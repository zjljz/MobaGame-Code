#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Combat/CombatGameplayEffects.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Hero/HeroDefinition.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/ParticipantState.h"
#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"

namespace
{
	class FManagedAbilityNetworkScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FManagedAbilityNetworkScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaManagedRole="), Role);
			FParse::Value(FCommandLine::Get(), TEXT("MobaManagedEvidence="), Evidence);
			FPaths::NormalizeDirectoryName(Evidence);
		}


	private:
		FString Read(const TCHAR* Name) const
		{
			FString Value;
			const FString Path = FPaths::Combine(Evidence, Name);
			if (IFileManager::Get().FileExists(*Path))
			{
				FFileHelper::LoadFileToString(Value, *Path);
			}

			return Value;
		}


	public:
		virtual bool Update() override
		{
			if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence)
				|| !(Evidence.Contains(TEXT("/AI/task-008/")) || Evidence.Contains(TEXT("/AI/task-009/")) || Evidence.Contains(TEXT("/AI/task-011/"))))
			{
				Test->AddError(TEXT("MobaManagedEvidence必须为本批任务AI内本次运行的独立绝对目录"));
				return true;
			}

			if (Role != TEXT("server") && Role != TEXT("initial") && Role != TEXT("observer") && Role != TEXT("restart"))
			{
				return Fail(TEXT("MobaManagedRole无效"));
			}

			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("本角色已有失败断言"));
			}

			for (const TCHAR* Peer : {TEXT("server"), TEXT("initial"), TEXT("observer"), TEXT("restart")})
			{
				if (!Read(*(FString(Peer) + TEXT("-failed"))).IsEmpty())
				{
					return Fail(TEXT("对端测试失败，见对应角色报告"));
				}
			}

			if (FPlatformTime::Seconds() - Started > 360)
			{
				return Fail(TEXT("等待可观察状态超过360秒"));
			}

			// Server测试完成后进程可能已关闭；此时Observer只需完成已验证场景的交接。
			if (Role == TEXT("observer") && Stage == 6 && !Read(TEXT("server-passed")).IsEmpty())
			{
				Signal(TEXT("observer-passed"));
				return FinishStep(true);
			}

			UWorld* World = nullptr;
			for (const auto& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			if (Role == TEXT("server"))
			{
				if (World->GetNetMode() != NM_DedicatedServer)
				{
					return false;
				}

				return FinishStep(Server(World));
			}

			if (World->GetNetMode() != NM_Client)
			{
				return false;
			}

			auto* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Hero = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
			if (!Controller || !Hero || Controller->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}

			if (Role == TEXT("observer"))
			{
				return FinishStep(Observer(World, Controller, Hero));
			}

			return FinishStep(Role == TEXT("initial") ? Initial(Controller, Hero) : Restart(Controller, Hero));
		}


	private:
		void Signal(const TCHAR* Name, const FString& Value = TEXT("ready"))
		{
			// 失败断言不能发布后续阶段或passed；统一在本次Update尾部传播失败。
			if (Test->HasAnyErrors() && !FString(Name).EndsWith(TEXT("-failed")))
			{
				return;
			}

			Test->TestTrue(TEXT("写入公开场景状态"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		bool FinishStep(bool bDone) { return Test->HasAnyErrors() ? Fail(TEXT("本阶段断言或标记写入失败")) : bDone; }

		bool Fail(const TCHAR* Why)
		{
			const FString Message = FString::Printf(TEXT("ManagedNetwork Role=%s Stage=%d Reason=%s"), *Role, Stage, Why);
			Test->AddError(Message);
			Signal(*(Role + TEXT("-failed")), Message);
			return true;
		}

		bool ReadIdentity()
		{
			TArray<FString> Fields;
			Read(TEXT("identity")).ParseIntoArray(Fields, TEXT("|"));
			if (Fields.Num() != 4 || !FGuid::Parse(Fields[0], Entity) || !FGuid::Parse(Fields[1], Participant))
			{
				return false;
			}

			OriginalEpoch = static_cast<uint32>(FCString::Strtoui64(*Fields[2], nullptr, 10));
			OriginalRevision = static_cast<uint32>(FCString::Strtoui64(*Fields[3], nullptr, 10));
			return OriginalEpoch > 0 && OriginalRevision > 0;
		}

		bool Ready(AHeroPlayerController* Controller, AHeroCharacter* Hero) const
		{
			return Controller->IsLocallyReady() && Hero->GetHeroAbilitySystem()->HasReplicatedLoadout(Hero->RequiredLoadoutRevision);
		}

		uint32 MarkerRevision(const TCHAR* Name) const { return static_cast<uint32>(FCString::Strtoui64(*Read(Name), nullptr, 10)); }

		bool Applied(const TCHAR* Operation, const FAbilitySourceResult& Result)
		{
			if (Result.Status == EAbilitySourceResult::Applied)
			{
				return true;
			}

			Test->AddError(FString::Printf(TEXT("%s Status=%d Reason=%s"), Operation, int32(Result.Status), *Result.Reason.ToString()));
			return false;
		}

		bool Rejected(AHeroPlayerController* Controller, FName Reason)
		{
			return Test->TestFalse(TEXT("真实RPC拒绝"), Controller->WasLastIntentAccepted()) && Test->TestEqual(TEXT("真实RPC拒绝原因"), Controller->GetLastIntentReason(), Reason);
		}

		bool CheckStableServer(AHeroCharacter* Hero, UHeroAbilitySystemComponent* Abilities, AParticipantState* State)
		{
			return Hero == OriginalHero && Abilities == OriginalASC && State == OriginalParticipant && Hero->EntityId == Entity && Hero->ParticipantId == Participant
				&& Hero->InitializationGeneration == 1;
		}

		bool CheckManagedSpecs(UHeroAbilitySystemComponent* Abilities, int32 Level)
		{
			const auto* Record = Abilities->FindSource(Entity);
			if (!Record || !Record->Definition || Record->Abilities.Num() != Record->Definition->Abilities.Num()
				|| Abilities->GetActivatableAbilities().Num() != Record->Definition->Abilities.Num())
			{
				return false;
			}

			for (auto Handle : Record->Abilities)
			{
				const auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
				if (!Spec || Spec->Level != Level)
				{
					return false;
				}
			}

			return true;
		}

		bool CheckCooldown(UHeroAbilitySystemComponent* Abilities, UWorld* World)
		{
			const auto* Effect = Abilities->GetActiveGameplayEffect(LongCooldown);
			return Effect && Effect->StartWorldTime == CooldownStarted && Effect->GetTimeRemaining(World->GetTimeSeconds()) > 0 && Effect->GetTimeRemaining(World->GetTimeSeconds()) < 60;
		}

		bool Server(UWorld* World)
		{
			if (!ReadIdentity())
			{
				return false;
			}

			AHeroCharacter* Hero = nullptr;
			AParticipantState* State = nullptr;
			int32 ParticipantCount = 0;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Entity)
				{
					Hero = *It;
				}
			}

			for (TActorIterator<AParticipantState> It(World); It; ++It)
			{
				if (It->ParticipantId == Participant)
				{
					State = *It;
					++ParticipantCount;
				}
			}

			if (!Hero || !State || ParticipantCount != 1)
			{
				return Fail(TEXT("原Hero或唯一Participant丢失"));
			}

			auto* Abilities = Hero->GetHeroAbilitySystem();
			if (Stage > 0 && !CheckStableServer(Hero, Abilities, State))
			{
				return Fail(TEXT("Hero/ASC/Participant/初始化身份改变"));
			}

			switch (Stage)
			{
			case 0:
			{
				if (Read(TEXT("observer-ready")).IsEmpty() || !Hero->bInputReady)
				{
					return false;
				}

				OriginalHero = Hero;
				OriginalASC = Abilities;
				OriginalParticipant = State;
				if (!Hero->GetDefinition() || !Hero->GetDefinition()->AbilitySet.IsValid())
				{
					return Fail(TEXT("Managed定义未加载"));
				}

				const auto* OriginalSet = Hero->GetDefinition()->AbilitySet.Get();
				if (!OriginalSet->GetPathName().Contains(TEXT("/Batch0811/")))
				{
					return Fail(TEXT("未通过Batch0811 Playlist启动"));
				}

				OriginalHandle = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
				auto* Replacement = NewObject<UAbilitySetDefinition>(Abilities);
				Replacement->Abilities = OriginalSet->Abilities;
				Replacement->Effects = OriginalSet->Effects;
				Replacement->bAllowCopy = OriginalSet->bAllowCopy;
				for (auto& Entry : Replacement->Abilities)
				{
					Entry.Level = 2;
				}

				if (!Applied(TEXT("Replace"), Abilities->ReplaceManagedSource(Entity, Replacement, Abilities->GetStateRevision())))
				{
					return true;
				}

				if (!CheckManagedSpecs(Abilities, 2) || Abilities->FindAbilitySpecFromHandle(OriginalHandle))
				{
					return Fail(TEXT("Replace未形成新等级与真实Spec"));
				}

				ManagedHandle = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
				Signal(TEXT("replaced"), FString::FromInt(Hero->RequiredLoadoutRevision));
				Stage = 1;
				return false;
			}
			case 1:
				if (Read(TEXT("replace-probes")).IsEmpty())
				{
					return false;
				}

				if (!Applied(TEXT("Remap"), Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary, MobaGameplayTags::Input_Slot_UnitAttack, Abilities->GetStateRevision())))
				{
					return true;
				}

				if (Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack) != ManagedHandle)
				{
					return Fail(TEXT("Remap没有保留Spec"));
				}

				Signal(TEXT("remapped"), FString::FromInt(Hero->RequiredLoadoutRevision));
				Stage = 2;
				return false;
			case 2:
			{
				bObservedSelfCooldown |= Hero->GetAttributes()->GetMana() == 80 && Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self);
				if (Read(TEXT("owner-cast")).IsEmpty() || Read(TEXT("observer-cost")).IsEmpty())
				{
					return false;
				}

				Test->TestEqual(TEXT("Server真实Self成本"), Hero->GetAttributes()->GetMana(), 80.f);
				Test->TestTrue(TEXT("Server新槽保持等级"), CheckManagedSpecs(Abilities, 2));
				Test->TestTrue(TEXT("Server真实观察过Self成本和激活冷却"), bObservedSelfCooldown);
				if (Test->HasAnyErrors())
				{
					return true;
				}

				auto Spec = Abilities->MakeOutgoingSpec(UUnitAttackCooldownEffect::StaticClass(), 1, Abilities->MakeEffectContext());
				if (!Spec.IsValid())
				{
					return Fail(TEXT("长冷却Spec创建失败"));
				}

				Spec.Data->SetDuration(60, true);
				LongCooldown = Abilities->ApplyGameplayEffectSpecToSelf(*Spec.Data);
				const auto* Cooldown = Abilities->GetActiveGameplayEffect(LongCooldown);
				if (!Cooldown)
				{
					return Fail(TEXT("长冷却真实应用失败"));
				}

				CooldownStarted = Cooldown->StartWorldTime;
				Signal(TEXT("cooldown-start"), FString::Printf(TEXT("%.9g"), Cooldown->StartServerWorldTime));
				auto* Damage = NewObject<UGameplayEffect>(World);
				Damage->DurationPolicy = EGameplayEffectDurationType::Instant;
				auto& Modifier = Damage->Modifiers.AddDefaulted_GetRef();
				Modifier.Attribute = UHeroAttributeSet::GetIncomingDamageAttribute();
				Modifier.ModifierOp = EGameplayModOp::Additive;
				Modifier.ModifierMagnitude = FScalableFloat(Hero->GetAttributes()->GetMaxHealth() + 1);
				Abilities->ApplyGameplayEffectToSelf(Damage, 1, Abilities->MakeEffectContext());
				Test->TestFalse(TEXT("真实伤害GE导致死亡"), Hero->IsAlive());
				Signal(TEXT("dead"));
				Stage = 3;
				return false;
			}
			case 3:
				if (!Hero->IsAlive() || Hero->GetLifeSnapshot().Generation < 2 || Read(TEXT("owner-dead")).IsEmpty() || Read(TEXT("observer-dead")).IsEmpty())
				{
					return false;
				}

				Test->TestTrue(TEXT("死亡复活保持新Spec/等级/来源"), CheckManagedSpecs(Abilities, 2));
				Test->TestEqual(TEXT("复活保留映射与Spec"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack), ManagedHandle);
				Test->TestTrue(TEXT("复活保留原剩余冷却"), CheckCooldown(Abilities, World));
				Test->TestEqual(TEXT("复活未重授来源"), Abilities->GetSourceCount(), 1);
				Signal(TEXT("respawned"));
				Stage = 4;
				return false;
			case 4:
				if (Read(TEXT("owner-respawned")).IsEmpty() || Read(TEXT("observer-respawned")).IsEmpty())
				{
					return false;
				}

				if (!Applied(TEXT("EmptyRemove"), Abilities->RemoveManagedSource(Entity, Abilities->GetStateRevision())))
				{
					return true;
				}

				Signal(TEXT("empty"), FString::FromInt(Hero->RequiredLoadoutRevision));
				Stage = 5;
				return false;
			case 5:
				if (Read(TEXT("owner-empty-ready")).IsEmpty() || !Hero->bInputReady)
				{
					return false;
				}

				Test->TestEqual(TEXT("Server空映射ready时没有Spec"), Abilities->GetActivatableAbilities().Num(), 0);
				Test->TestTrue(TEXT("Server空映射快照完整"), Abilities->HasReplicatedLoadout(Hero->RequiredLoadoutRevision));
				if (Test->HasAnyErrors())
				{
					return true;
				}

				if (!Applied(TEXT("RestoreGrant"), Abilities->GrantManagedSource(Hero->GetDefinition()->AbilitySet.Get(), Entity, Abilities->GetStateRevision())))
				{
					return true;
				}

				for (const auto Handle : Abilities->FindSource(Entity)->Abilities)
				{
					if (!Applied(TEXT("RestoreUpgrade"), Abilities->UpgradeManagedAbility(Handle, 2, Abilities->GetStateRevision())))
					{
						return true;
					}
				}

				if (!Applied(TEXT("RestoreRemap"), Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary, MobaGameplayTags::Input_Slot_UnitAttack, Abilities->GetStateRevision())))
				{
					return true;
				}

				ManagedHandle = Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack);
				FinalRevision = Hero->RequiredLoadoutRevision;
				Signal(TEXT("restored"), FString::FromInt(FinalRevision));
				Stage = 6;
				return false;
			case 6:
			{
				if (Read(TEXT("owner-restored")).IsEmpty())
				{
					return false;
				}

				auto* TemporarySet = NewObject<UAbilitySetDefinition>(Abilities);
				TemporarySet->Effects.Add(UManagedAbilityPassiveEffect::StaticClass());
				TemporarySource = FGuid::NewGuid();
				if (!Applied(TEXT("TemporaryGrant"), Abilities->GrantManagedSource(TemporarySet, TemporarySource, Abilities->GetStateRevision(), 15)))
				{
					return true;
				}

				const auto* Temporary = Abilities->FindSource(TemporarySource);
				if (!Temporary || Temporary->Effects.Num() != 1)
				{
					return Fail(TEXT("临时来源GE记录缺失"));
				}

				TemporaryEffect = Temporary->Effects[0];
				Test->TestNotNull(TEXT("退出前临时GE存在"), Abilities->GetActiveGameplayEffect(TemporaryEffect));
				Signal(TEXT("exit-initial"));
				Stage = 7;
				return false;
			}
			case 7:
				if (State->bConnected)
				{
					return false;
				}

				Test->TestNotNull(TEXT("真实断线时临时来源尚未到期"), Abilities->FindSource(TemporarySource));
				DetachedEpoch = State->ConnectionEpoch;
				Signal(TEXT("detached"), FString::FromInt(DetachedEpoch));
				Stage = 8;
				return false;
			case 8:
				if (Abilities->FindSource(TemporarySource))
				{
					return false;
				}

				if (Read(TEXT("observer-offline")).IsEmpty())
				{
					return false;
				}

				Test->TestFalse(TEXT("临时到期发生在原玩家离线期间"), State->bConnected);
				Test->TestNull(TEXT("离线到期实际移除GE"), Abilities->GetActiveGameplayEffect(TemporaryEffect));
				Test->TestTrue(TEXT("到期保留原新能力状态"), CheckManagedSpecs(Abilities, 2));
				Test->TestEqual(TEXT("仅剩正式来源"), Abilities->GetSourceCount(), 1);
				Test->TestTrue(TEXT("离线冷却继续"), CheckCooldown(Abilities, World));
				FinalStateRevision = Abilities->GetStateRevision();
				Signal(TEXT("offline-expired"));
				Stage = 9;
				return false;
			case 9:
				if (!State->bConnected || !Hero->bInputReady || Hero->ConnectionEpoch <= OriginalEpoch)
				{
					return false;
				}

				Test->TestTrue(TEXT("重启同一Hero/ASC/Participant"), CheckStableServer(Hero, Abilities, State));
				Test->TestTrue(TEXT("重启原新能力等级"), CheckManagedSpecs(Abilities, 2));
				Test->TestEqual(TEXT("重启同一来源数"), Abilities->GetSourceCount(), 1);
				Test->TestEqual(TEXT("重启未重授Spec"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack), ManagedHandle);
				Test->TestEqual(TEXT("重启未推进来源状态版本"), Abilities->GetStateRevision(), FinalStateRevision);
				Test->TestEqual(TEXT("重启未推进槽版本"), Hero->RequiredLoadoutRevision, FinalRevision);
				Test->TestTrue(TEXT("重启保留原冷却剩余时间"), CheckCooldown(Abilities, World));
				Test->TestTrue(TEXT("ActorInfo绑定新Controller"), Abilities->AbilityActorInfo.IsValid() && Abilities->AbilityActorInfo->PlayerController.Get() == Hero->GetController());
				Signal(TEXT("server-rebound"));
				Stage = 10;
				return false;
			case 10:
				if (Read(TEXT("restart-passed")).IsEmpty() || Read(TEXT("observer-restart")).IsEmpty())
				{
					return false;
				}

				Signal(TEXT("server-passed"));
				Stage = 11;
				return false;
			case 11:
				if (Read(TEXT("observer-passed")).IsEmpty())
				{
					return false;
				}

				return true;
			}

			return false;
		}

		bool Initial(AHeroPlayerController* Controller, AHeroCharacter* Hero)
		{
			auto* Abilities = Hero->GetHeroAbilitySystem();
			switch (Stage)
			{
			case 0:
				if (!Ready(Controller, Hero))
				{
					return false;
				}

				Entity = Hero->EntityId;
				Participant = Hero->ParticipantId;
				OriginalEpoch = Hero->ConnectionEpoch;
				OriginalRevision = Hero->RequiredLoadoutRevision;
				Signal(TEXT("identity"), FString::Printf(TEXT("%s|%s|%u|%u"), *Entity.ToString(), *Participant.ToString(), OriginalEpoch, OriginalRevision));
				Stage = 1;
				return false;
			case 1:
				if (!MarkerRevision(TEXT("replaced")) || Hero->RequiredLoadoutRevision != MarkerRevision(TEXT("replaced")) || !Ready(Controller, Hero))
				{
					return false;
				}

				ReplacedRevision = Hero->RequiredLoadoutRevision;
				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(Entity, OriginalEpoch, OriginalRevision, MobaGameplayTags::Input_Slot_Primary);
				Stage = 2;
				return false;
			case 2:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Rejected(Controller, TEXT("Revision")))
				{
					return true;
				}

				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(Entity, OriginalEpoch - 1, ReplacedRevision, MobaGameplayTags::Input_Slot_Primary);
				Stage = 3;
				return false;
			case 3:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Rejected(Controller, TEXT("Epoch")))
				{
					return true;
				}

				Test->TestEqual(TEXT("Replace拒绝不扣资源"), Hero->GetAttributes()->GetMana(), 100.f);
				Signal(TEXT("replace-probes"));
				Stage = 4;
				return false;
			case 4:
				if (!MarkerRevision(TEXT("remapped")) || Hero->RequiredLoadoutRevision != MarkerRevision(TEXT("remapped")) || !Ready(Controller, Hero))
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(Entity, OriginalEpoch, ReplacedRevision, MobaGameplayTags::Input_Slot_UnitAttack);
				Stage = 5;
				return false;
			case 5:
			{
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Rejected(Controller, TEXT("Revision")))
				{
					return true;
				}

				const auto* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack));
				if (!Spec || !Spec->Ability || !Spec->Ability->IsA<USelfCommitAbility>() || Spec->Level != 2)
				{
					return Fail(TEXT("Owner未收到新槽Self/等级"));
				}

				Ack = Controller->GetIntentResultSequence();
				Test->TestTrue(TEXT("原生E键进入输入系统"), Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Pressed, FPlatformTime::Cycles64())));
				Stage = 6;
				return false;
			}
			case 6:
				if (!bInputReleased)
				{
					Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Released, FPlatformTime::Cycles64()));
					bInputReleased = true;
				}

				bObservedSelfCooldown |= Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self);
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestTrue(TEXT("拥有客户端新槽真实RPC成功"), Controller->WasLastIntentAccepted());
				if (Test->HasAnyErrors())
				{
					return true;
				}

				if (Hero->GetAttributes()->GetMana() != 80 || !bObservedSelfCooldown)
				{
					return false;
				}

				Signal(TEXT("owner-cast"));
				Stage = 7;
				return false;
			case 7:
				if (Hero->IsAlive() || Hero->GetAttributes()->GetHealth() != 0)
				{
					return false;
				}

				Test->TestEqual(TEXT("拥有客户端观察死亡血量"), Hero->GetAttributes()->GetHealth(), 0.f);
				Signal(TEXT("owner-dead"));
				Stage = 8;
				return false;
			case 8:
				if (Read(TEXT("respawned")).IsEmpty() || !Hero->IsAlive() || Hero->GetLifeSnapshot().Generation != 2 || !Ready(Controller, Hero)
					|| !Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Test->TestTrue(TEXT("拥有客户端复活保留冷却"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack));
				Signal(TEXT("owner-respawned"));
				Stage = 9;
				return false;
			case 9:
				if (!MarkerRevision(TEXT("empty")) || Hero->RequiredLoadoutRevision != MarkerRevision(TEXT("empty")) || !Ready(Controller, Hero) || !Abilities->GetSlots().IsEmpty()
					|| !Abilities->GetActivatableAbilities().IsEmpty())
				{
					return false;
				}

				Test->TestEqual(TEXT("拥有客户端空槽ready"), Abilities->GetSlots().Num(), 0);
				Test->TestEqual(TEXT("拥有客户端空Spec已复制"), Abilities->GetActivatableAbilities().Num(), 0);
				Signal(TEXT("owner-empty-ready"));
				Stage = 10;
				return false;
			case 10:
			{
				if (!MarkerRevision(TEXT("restored")) || Hero->RequiredLoadoutRevision != MarkerRevision(TEXT("restored")) || !Ready(Controller, Hero))
				{
					return false;
				}

				const auto* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack));
				if (!Spec || Spec->Level != 2 || !Spec->Ability || !Spec->Ability->IsA<USelfCommitAbility>())
				{
					return Fail(TEXT("恢复后Owner状态不完整"));
				}

				Signal(TEXT("owner-restored"));
				Stage = 11;
				return false;
			}
			case 11:
				if (Read(TEXT("exit-initial")).IsEmpty())
				{
					return false;
				}

				Signal(TEXT("initial-passed"));
				Test->AddInfo(TEXT("初始客户端测试完成；runner的TestExit退出此进程，等待server offline-expired后以相同UserDir和-MobaResume启动restart。"));
				return true;
			}

			return false;
		}

		bool Observer(UWorld* World, AHeroPlayerController* Controller, AHeroCharacter* LocalHero)
		{
			if (!ReadIdentity() || !Ready(Controller, LocalHero))
			{
				return false;
			}

			AHeroCharacter* Hero = nullptr;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Entity)
				{
					Hero = *It;
				}
			}

			if (!Hero || Hero == LocalHero)
			{
				return false;
			}

			auto* Abilities = Hero->GetHeroAbilitySystem();
			if (Stage > 0 && (Hero != OriginalHero || Abilities != OriginalASC))
			{
				return Fail(TEXT("Observer远端Hero/ASC被重建"));
			}

			switch (Stage)
			{
			case 0:
				OriginalHero = Hero;
				OriginalASC = Abilities;
				Signal(TEXT("observer-ready"));
				Stage = 1;
				return false;
			case 1:
				bObservedSelfCooldown |= Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self);
				if (Hero->GetAttributes()->GetMana() != 80 || !bObservedSelfCooldown)
				{
					return false;
				}

				Test->TestEqual(TEXT("Observer收到真实资源成本"), Hero->GetAttributes()->GetMana(), 80.f);
				Signal(TEXT("observer-cost"));
				Stage = 2;
				return false;
			case 2:
				if (Hero->IsAlive() || Hero->GetAttributes()->GetHealth() != 0)
				{
					return false;
				}

				Test->TestEqual(TEXT("Observer收到死亡血量"), Hero->GetAttributes()->GetHealth(), 0.f);
				Signal(TEXT("observer-dead"));
				Stage = 3;
				return false;
			case 3:
				if (!Hero->IsAlive() || Hero->GetLifeSnapshot().Generation != 2 || !Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Test->TestEqual(TEXT("Observer同一实体复活"), Hero->EntityId, Entity);
				Test->TestEqual(TEXT("Observer死亡一次"), Hero->GetLifeSnapshot().DeathCount, uint32(1));
				Signal(TEXT("observer-respawned"));
				Stage = 4;
				return false;
			case 4:
				if (Read(TEXT("detached")).IsEmpty() || Hero->bInputReady)
				{
					return false;
				}

				Test->TestTrue(TEXT("Observer离线Hero仍存在且存活"), Hero->IsAlive());
				Signal(TEXT("observer-offline"));
				Stage = 5;
				return false;
			case 5:
				if (Read(TEXT("offline-expired")).IsEmpty() || Read(TEXT("server-rebound")).IsEmpty() || !Hero->bInputReady || Hero->ConnectionEpoch <= OriginalEpoch
					|| !Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Test->TestEqual(TEXT("Observer重启仍原初始化代次"), Hero->InitializationGeneration, uint32(1));
				Test->TestTrue(TEXT("Observer重启原冷却仍可见"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack));
				Signal(TEXT("observer-restart"));
				Stage = 6;
				return false;
			case 6:
				if (Read(TEXT("server-passed")).IsEmpty())
				{
					return false;
				}

				Signal(TEXT("observer-passed"));
				return true;
			}

			return false;
		}

		bool Restart(AHeroPlayerController* Controller, AHeroCharacter* Hero)
		{
			if (!ReadIdentity() || !Ready(Controller, Hero) || Read(TEXT("offline-expired")).IsEmpty())
			{
				return false;
			}

			auto* Abilities = Hero->GetHeroAbilitySystem();
			if (Stage == 0)
			{
				// Life、Spec与GE分属不同复制载荷；等待可见状态收敛后再核对来源身份。
				if (Hero->GetLifeSnapshot().Generation != 2 || Hero->GetLifeSnapshot().DeathCount != 1 || !Hero->GetDefinition() || !Hero->GetDefinition()->AbilitySet.IsValid()
					|| Abilities->GetActivatableAbilities().Num() != Hero->GetDefinition()->AbilitySet->Abilities.Num())
				{
					return false;
				}

				const auto Cooldowns = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_UnitAttack)));
				if (Cooldowns.IsEmpty())
				{
					return false;
				}

				Test->TestEqual(TEXT("进程重启合法票据找回原Entity"), Hero->EntityId, Entity);
				Test->TestEqual(TEXT("进程重启原Participant"), Hero->ParticipantId, Participant);
				Test->TestTrue(TEXT("进程重启新Epoch"), Hero->ConnectionEpoch > OriginalEpoch);
				Test->TestEqual(TEXT("进程重启未重做初始化"), Hero->InitializationGeneration, uint32(1));
				Test->TestEqual(TEXT("进程重启原槽版本"), Hero->RequiredLoadoutRevision, MarkerRevision(TEXT("restored")));
				Test->TestEqual(TEXT("进程重启保留实际定义Spec数"), Abilities->GetActivatableAbilities().Num(), Hero->GetDefinition()->AbilitySet->Abilities.Num());
				const auto* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack));
				Test->TestTrue(TEXT("进程重启恢复新槽等级"), Spec && Spec->Level == 2 && Spec->Ability && Spec->Ability->IsA<USelfCommitAbility>());
				if (Cooldowns.Num() != 1)
				{
					return Fail(TEXT("重启拥有客户端缺原持续冷却"));
				}

				const auto* Cooldown = Abilities->GetActiveGameplayEffect(Cooldowns[0]);
				if (!Cooldown)
				{
					return false;
				}

				Test->TestEqual(TEXT("重启冷却原服务器开始时间"), Cooldown->StartServerWorldTime, FCString::Atof(*Read(TEXT("cooldown-start"))), 0.02f);
				Test->TestEqual(TEXT("重启保持已经发生的一次死亡"), Hero->GetLifeSnapshot().DeathCount, uint32(1));
				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(Entity, OriginalEpoch, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack);
				Stage = 1;
				return false;
			}

			if (Stage == 1)
			{
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Rejected(Controller, TEXT("Epoch")))
				{
					return true;
				}

				Stage = 2;
			}

			if (Read(TEXT("server-rebound")).IsEmpty() || Read(TEXT("observer-restart")).IsEmpty())
			{
				return false;
			}

			Signal(TEXT("restart-passed"));
			return true;
		}

		FAutomationTestBase* Test;


	private:
		double Started;

		FString Role, Evidence;

		int32 Stage = 0;

		FGuid Entity, Participant, TemporarySource;

		uint32 OriginalEpoch = 0, OriginalRevision = 0, ReplacedRevision = 0, FinalRevision = 0;

		uint32 FinalStateRevision = 0, DetachedEpoch = 0, Ack = 0;

		TWeakObjectPtr<AHeroCharacter> OriginalHero;

		TWeakObjectPtr<UHeroAbilitySystemComponent> OriginalASC;

		TWeakObjectPtr<AParticipantState> OriginalParticipant;

		FGameplayAbilitySpecHandle OriginalHandle, ManagedHandle;

		FActiveGameplayEffectHandle LongCooldown, TemporaryEffect;

		float CooldownStarted = 0;

		bool bObservedSelfCooldown = false;

		bool bInputReleased = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FManagedAbilityNetworkTest, "Moba.Network.ManagedAbility", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityNetworkTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FManagedAbilityNetworkScenario>(this));
	return true;
}

#endif
