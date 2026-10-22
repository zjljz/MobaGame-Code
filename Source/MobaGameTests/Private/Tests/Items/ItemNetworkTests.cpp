#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatStatusEffects.h"
#include "Experience/ExperienceDefinitions.h"
#include "Items/ItemDefinition.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Session/MobaSessionSubsystem.h"
#include "Items/ItemInventoryComponent.h"
#include "MobaGameplayTags.h"
#include "UObject/UObjectIterator.h"

namespace
{
/** 显式测试工作区只约束输出根；不修改引擎ProjectDir或内容加载。 */
bool ResolveItemNetworkEvidenceRoot(FString& Workspace, FString& Root)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	FPaths::CollapseRelativeDirectories(ProjectDir);
	const bool bExplicitWorkspace = FParse::Value(FCommandLine::Get(), TEXT("MobaItemUIWorkspace="), Workspace);
	if (bExplicitWorkspace)
	{
		if (Workspace.IsEmpty() || FPaths::IsRelative(Workspace))
		{
			return false;
		}

		FPaths::NormalizeDirectoryName(Workspace);
		FPaths::CollapseRelativeDirectories(Workspace);
		const FString ProjectFile = FPaths::Combine(Workspace, TEXT("MobaGame.uproject"));
		FString Descriptor;
		if (FString(FApp::GetProjectName()) != TEXT("MobaGame") || !FFileHelper::LoadFileToString(Descriptor, *ProjectFile)
			|| !Descriptor.Contains(TEXT("\"FileVersion\"")) || !Descriptor.Contains(TEXT("\"Name\": \"MobaGame\""))
			|| (ProjectDir != Workspace && !FPaths::IsUnderDirectory(ProjectDir, FPaths::Combine(Workspace, TEXT("AI")))))
		{
			return false;
		}
	}
	else
	{
		Workspace = ProjectDir;
	}

	Root = FPaths::Combine(Workspace, bExplicitWorkspace ? TEXT("AI/task-011/integration-012-015-20261009") : TEXT("AI/task-011/integration-012-015-20261009"));
	FPaths::NormalizeDirectoryName(Root);
	FPaths::CollapseRelativeDirectories(Root);
	return true;
}


	class FItemNetworkScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FItemNetworkScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaItemRole="), Role);
			FParse::Value(FCommandLine::Get(), TEXT("MobaItemEvidence="), Evidence);
			FPaths::NormalizeDirectoryName(Evidence);
			bNoItems = FParse::Param(FCommandLine::Get(), TEXT("MobaNoItems"));
			bUIRecovery = FParse::Param(FCommandLine::Get(), TEXT("MobaItemUIRecovery"));
			FParse::Value(FCommandLine::Get(), TEXT("MobaItemUIRunToken="), UIRunToken);
			if (bUIRecovery)
			{
				FPaths::CollapseRelativeDirectories(Evidence);
				FString Workspace, Root;
				if (IsUIBoundaryValid() && ResolveItemNetworkEvidenceRoot(Workspace, Root))
				{
					Test->AddInfo(FString::Printf(TEXT("ItemUIBoundary ProjectDir=%s Workspace=%s Root=%s Role=%s Token=%s"),
						*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), *Workspace, *Root, *Role, *UIRunToken));
				}
			}
		}


	public:
		bool Update() override
		{
			if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence) || !Evidence.Contains(TEXT("/AI/task-011/")))
			{
				return Fail(TEXT("输出路径边界"));
			}

			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("本阶段断言失败"));
			}

			if (bUIRecovery)
			{
				if (!IsUIBoundaryValid())
				{
					return Fail(TEXT("UI fixture Evidence/role/run token无效"));
				}
			}

			if (FPlatformTime::Seconds() - Started > 240)
			{
				return Fail(TEXT("状态谓词超时"));
			}

			for (const TCHAR* Peer : {TEXT("server"), TEXT("initial"), TEXT("observer"), TEXT("restart")})
			{
				if (Has(*(FString(Peer) + TEXT("-failed"))))
				{
					return Fail(TEXT("对端失败"));
				}
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

			if (bNoItems)
			{
				return NoItems(World);
			}

			if (Role == TEXT("server"))
			{
				return World->GetNetMode() == NM_DedicatedServer && Server(World);
			}

			if (Role == TEXT("observer") && Has(TEXT("server-passed")))
			{
				if (!UIPhaseReady(TEXT("ui-observer-isolated")))
				{
					return false;
				}

				Mark(TEXT("observer-passed"));
				return true;
			}

			if (Role == TEXT("initial") && Stage == 41)
			{
				if (World->GetNetMode() == NM_Client || !Has(TEXT("loss-detached")))
				{
					return false;
				}

				if (!UIPhaseReady(TEXT("ui-loss-cleared")))
				{
					return false;
				}

				World->GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
				GEngine->SetClientTravel(World, *Address, TRAVEL_Absolute);
				Stage = 42;
				return false;
			}

			if (World->GetNetMode() != NM_Client)
			{
				return false;
			}

			auto* PC = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Hero = PC ? Cast<AHeroCharacter>(PC->GetPawn()) : nullptr;
			if (!Hero || PC->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}

			if (Role == TEXT("restart"))
			{
				return PC->IsLocallyReady() && Reconnected(PC, Hero, true);
			}

			if (!Has(*(Role + TEXT("-identity"))))
			{
				if (!PC->IsLocallyReady())
				{
					return false;
				}

				Mark(*(Role + TEXT("-identity")), Hero->EntityId.ToString());
			}

			auto* Owner = Find(World, Read(TEXT("initial-identity")));
			if (!Owner || !Has(TEXT("observer-identity")))
			{
				return false;
			}

			if (Role == TEXT("observer"))
			{
				return Observe(Owner);
			}

			if (Stage == 42)
			{
				return PC->IsLocallyReady() && Reconnected(PC, Hero, false);
			}

			if (Stage == 0 && !UIPhaseReady(TEXT("ui-initial-mounted")))
			{
				return false;
			}

			return Initial(PC, Hero);
		}


	private:
		FString Read(const TCHAR* Name) const
		{
			FString Value;
			FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
			return Value;
		}


	private:
		bool Has(const TCHAR* Name) const { return IFileManager::Get().FileExists(*FPaths::Combine(Evidence, Name)); }

		/** UI只通过本轮公开测试标记参与等待；默认场景不访问UI或改变推进顺序。 */
		bool UIPhaseReady(const TCHAR* Name) const
		{
			return !bUIRecovery || Read(Name) == UIRunToken;
		}

		bool IsUIBoundaryValid() const
		{
			FGuid Token;
			FString Workspace, Root;
			if (!ResolveItemNetworkEvidenceRoot(Workspace, Root))
			{
				return false;
			}
			return (Role == TEXT("server") || Role == TEXT("initial") || Role == TEXT("observer") || Role == TEXT("restart"))
				&& !FPaths::IsRelative(Evidence) && Evidence.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)
				&& FGuid::ParseExact(UIRunToken, EGuidFormats::Digits, Token) && Token.IsValid();
		}

		void Mark(const TCHAR* Name, const FString& Value = TEXT("ready"))
		{
			if (bUIRecovery && !IsUIBoundaryValid())
			{
				return;
			}

			if (Test->HasAnyErrors() && !FString(Name).EndsWith(TEXT("failed")))
			{
				return;
			}

			Test->TestTrue(TEXT("保存公开阶段谓词"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		bool Fail(const TCHAR* Reason)
		{
			const FString Message = FString::Printf(TEXT("ItemNetwork Role=%s Stage=%d Reason=%s"), *Role, Stage, Reason);
			Test->AddError(Message);
			Mark(*(Role + TEXT("-failed")), Message);
			return true;
		}

		AHeroCharacter* Find(UWorld* World, const FString& Identity)
		{
			FGuid Id;
			if (!FGuid::Parse(Identity, Id))
			{
				return nullptr;
			}

			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Id)
				{
					return *It;
				}
			}

			return nullptr;
		}

		FGuid First(AHeroCharacter* Hero, const TCHAR* Id)
		{
			const auto* Found = Hero->GetInventory()->GetSnapshot().Items.FindByPredicate([Id](const auto& Item) { return Item.ItemId == Id; });
			return Found ? Found->Id : FGuid();
		}

		FString InventorySignature(AHeroCharacter* Hero)
		{
			FString Value;
			for (const auto& Item : Hero->GetInventory()->GetSnapshot().Items)
			{
				Value += FString::Printf(TEXT("%s:%s:%d:%d:%s|"), *Item.Id.ToString(), *Item.ItemId.ToString(), Item.Quantity, Item.Slot, *Item.ActiveAbility.ToString());
			}

			return Value;
		}

		bool Coherent(AHeroPlayerController* PC, AHeroCharacter* Hero) const
		{
			auto* PS = PC->GetPlayerState<AParticipantState>();
			return PS && PS->GetEconomy().Revision == Hero->GetInventory()->GetSnapshot().Revision
				&& Hero->GetHeroAbilitySystem()->GetStateRevision() >= Hero->GetInventory()->GetSnapshot().AbilityStateRevision;
		}

		FString SourceSignature(AHeroCharacter* Hero)
		{
			FString Value;
			auto* ASC = Hero->GetHeroAbilitySystem();
			for (const auto& Item : Hero->GetInventory()->GetSnapshot().Items)
			{
				const auto* Source = ASC->FindSource(Item.Id);
				if (!Test->TestNotNull(TEXT("每个实例都有原来源"), Source))
				{
					continue;
				}

				Value += Item.Id.ToString() + TEXT(":") + GetPathNameSafe(Source->Definition) + TEXT(":");
				Test->TestFalse(TEXT("持久来源没有PendingRemove"), Source->bPendingRemove);
				for (const auto Handle : Source->Abilities)
				{
					Test->TestNotNull(TEXT("原能力Spec仍存在"), ASC->FindAbilitySpecFromHandle(Handle));
					Value += TEXT("A") + Handle.ToString();
				}

				for (const auto Handle : Source->Effects)
				{
					Test->TestNotNull(TEXT("原被动GE仍存在"), ASC->GetActiveGameplayEffect(Handle));
					Value += TEXT("E") + Handle.ToString();
				}

				Value += TEXT("|");
			}

			return Value;
		}

		FItemRequest Intent(AHeroCharacter* Hero, uint32 Sequence)
		{
			FItemRequest Request;
			Request.Entity = Hero->EntityId;
			Request.Epoch = Hero->ConnectionEpoch;
			Request.Sequence = Sequence;
			Request.Revision = Hero->GetInventory()->GetSnapshot().Revision;
			Request.Operation = EItemOperation::Buy;
			Request.ItemId = TEXT("ArmorPlate");
			return Request;
		}

		FActiveGameplayEffectHandle Apply(AHeroCharacter* Hero, const TCHAR* Name, float Duration = 0)
		{
			const FString Prefix = FString(TEXT("/MobaClassic/Tests/Batch0811/Effects/GE_")) + Name;
			auto* Class = LoadClass<UGameplayEffect>(nullptr, *(Prefix + TEXT(".GE_") + Name + TEXT("_C")));
			auto* ASC = Hero->GetHeroAbilitySystem();
			auto Spec = ASC->MakeOutgoingSpec(Class, 1, ASC->MakeEffectContext());
			if (!Spec.IsValid())
			{
				Test->AddError(TEXT("已Cook的效果Spec"));
				return {};
			}

			if (Duration > 0)
			{
				Spec.Data->SetDuration(Duration, true);
			}

			return ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}

		bool NoItems(UWorld* World)
		{
			auto* Experience = World->GetSubsystem<UExperienceSubsystem>();
			if (Experience->GetSnapshot().State != EExperienceState::ExperienceReady)
			{
				return false;
			}

			Test->TestEqual(TEXT("真实NoItems独立入口"), Experience->GetExperience()->GetFName(), FName(TEXT("Experience_NoItems")));
			Test->TestTrue(TEXT("禁用没有目录软引用"), Experience->GetExperience()->ItemCatalog.IsNull());
			int32 Catalogs = 0, Items = 0;
			for (TObjectIterator<UItemCatalogDefinition> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject))
				{
					++Catalogs;
				}
			}

			for (TObjectIterator<UItemDefinition> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject))
				{
					++Items;
				}
			}

			Test->TestEqual(TEXT("新进程实际未驻留ItemCatalog"), Catalogs, 0);
			Test->TestEqual(TEXT("新进程实际未驻留ItemDefinition"), Items, 0);
			if (Role == TEXT("server"))
			{
				if (!Has(TEXT("initial-passed")) || !Has(TEXT("observer-passed")))
				{
					return false;
				}

				int32 Count = 0;
				for (TActorIterator<AHeroCharacter> It(World); It; ++It)
				{
					++Count;
					Test->TestTrue(TEXT("禁用无库存"), It->GetInventory()->GetSnapshot().Items.IsEmpty());
				}

				Test->TestEqual(TEXT("禁用也真实两个Game准入"), Count, 2);
				Mark(TEXT("server-passed"));
				return true;
			}

			auto* PC = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Hero = PC ? Cast<AHeroCharacter>(PC->GetPawn()) : nullptr;
			if (!PC || !Hero || !PC->IsLocallyReady())
			{
				return false;
			}

			if (Stage == 0)
			{
				Mark(*(Role + TEXT("-identity")), Hero->EntityId.ToString());
				PC->MobaBuy(TEXT("ArmorPlate"));
				Stage = 1;
				return false;
			}

			if (PC->GetLastItemResult().Sequence != 1)
			{
				return false;
			}

			Test->TestEqual(TEXT("普通命令真实RPC禁用拒绝"), PC->GetLastItemResult().Reason, FName(TEXT("ItemsDisabled")));
			Test->TestFalse(TEXT("禁用没有成功交易"), PC->GetLastItemResult().bAccepted);
			if (PC->GetPlayerState<AParticipantState>()->GetEconomy().Gold != Experience->GetExperience()->InitialGold)
			{
				return false;
			}

			Test->TestEqual(TEXT("禁用交易不改变初始金币配置"), PC->GetPlayerState<AParticipantState>()->GetEconomy().Gold, 1000);
			Mark(*(Role + TEXT("-passed")));
			return Has(TEXT("server-passed"));
		}

		bool Server(UWorld* World)
		{
			auto* Owner = Find(World, Read(TEXT("initial-identity")));
			auto* Observer = Find(World, Read(TEXT("observer-identity")));
			if (!Owner || !Observer)
			{
				return false;
			}

			auto* ASC = Owner->GetHeroAbilitySystem();
			auto* PS = Cast<AParticipantState>(Owner->GetPlayerState());
			if (Stage == 0)
			{
				if (!Owner->bInputReady || !Observer->bInputReady || !PS)
				{
					return false;
				}

				SavedParticipant = PS;
				SavedHero = Owner;
				SavedASC = ASC;
				BaseSources = ASC->GetSourceCount();
				BaseSpecs = ASC->GetActivatableAbilities().Num();
				Test->TestEqual(TEXT("首次参赛只发1000"), PS->GetEconomy().Gold, 1000);
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 40);
				Mark(TEXT("commands-go"));
				Stage = 1;
				return false;
			}

			PS = SavedParticipant.Get();
			if (!PS)
			{
				return Fail(TEXT("丢失Participant"));
			}

			if (Stage == 1)
			{
				if (!Has(TEXT("commands-done")) || !Has(TEXT("observer-shield")))
				{
					return false;
				}

				Test->TestEqual(TEXT("服务器真实最终金币"), PS->GetEconomy().Gold, 0);
				Test->TestEqual(TEXT("服务器真实库存4槽"), Owner->GetInventory()->GetSnapshot().Items.Num(), 4);
				Test->TestEqual(TEXT("服务器来源相等"), ASC->GetSourceCount(), BaseSources + 4);
				Test->TestEqual(TEXT("原英雄Specs加三个物品Specs"), ASC->GetActivatableAbilities().Num(), BaseSpecs + 3);
				SavedInventory = InventorySignature(Owner);
				Mark(TEXT("saved-inventory"), SavedInventory);
				SavedSources = SourceSignature(Owner);
				Mark(TEXT("saved-sources"), SavedSources);
				const auto HealCooldowns = ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_ItemHeal)));
				if (!Test->TestEqual(TEXT("实际药剂五秒GE正在计时"), HealCooldowns.Num(), 1))
				{
					return Fail(TEXT("缺实际Heal冷却"));
				}

				ActualHealCooldown = HealCooldowns[0];
				SavedRevision = Owner->GetInventory()->GetSnapshot().Revision;
				// 5秒族已由真实物品提交验证；延长一个实际GE Spec只用于跨默认网络超时的原时钟证明。
				ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_ItemShield));
				LongCooldown = Apply(Owner, TEXT("ShieldItemCooldown"), 180);
				Test->TestTrue(TEXT("原生长时钟夹具有效"), LongCooldown.IsValid());
				Mark(TEXT("clock-start"), FString::SanitizeFloat(World->GetTimeSeconds()));
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
				Test->TestTrue(TEXT("同实体死亡"), Owner->EnterDeath(ASC->MakeEffectContext()));
				Test->TestEqual(TEXT("死亡清盾"), Owner->GetAttributes()->GetShield(), 0.f);
				Test->TestEqual(TEXT("死亡保持每个原来源及GE/Spec"), SourceSignature(Owner), SavedSources);
				Mark(TEXT("dead-go"));
				Stage = 2;
				return false;
			}

			if (Stage == 2)
			{
				if (!Owner->IsAlive() || !Has(TEXT("initial-dead")) || !Has(TEXT("observer-dead")))
				{
					return false;
				}

				Test->TestEqual(TEXT("复活金币不重发"), PS->GetEconomy().Gold, 0);
				Test->TestEqual(TEXT("复活实例/数量/句柄保持"), InventorySignature(Owner), SavedInventory);
				Test->TestEqual(TEXT("复活保持每个原来源及GE/Spec"), SourceSignature(Owner), SavedSources);
				Test->TestNotNull(TEXT("死亡复活保留原冷却handle"), ASC->GetActiveGameplayEffect(LongCooldown));
				Mark(TEXT("loss-go"));
				Stage = 3;
				return false;
			}

			if (Stage == 3)
			{
				if (!Has(TEXT("loss-start")))
				{
					return false;
				}

				if (!bDotApplied)
				{
					OfflineDot = Apply(Owner, TEXT("DamageOverTime"));
					bDotApplied = true;
					Test->TestTrue(TEXT("断网后服务器持续模拟DoT"), OfflineDot.IsValid());
				}

				if (PS->bConnected)
				{
					return false;
				}

				if (ASC->GetActiveGameplayEffect(OfflineDot))
				{
					return false;
				}

				Test->TestTrue(TEXT("真实断网DoT三tick乘0.9"), FMath::IsNearlyEqual(Owner->GetAttributes()->GetHealth(), 86.5f));
				Test->TestEqual(TEXT("断网保留原库存"), InventorySignature(Owner), SavedInventory);
				Test->TestEqual(TEXT("断网保持每个原来源及GE/Spec"), SourceSignature(Owner), SavedSources);
				Test->TestEqual(TEXT("断网保留原Gold"), PS->GetEconomy().Gold, 0);
				Test->TestNotNull(TEXT("超时后原冷却handle仍在"), ASC->GetActiveGameplayEffect(LongCooldown));
				Test->TestNull(TEXT("真实药剂五秒冷却离线自然到期"), ASC->GetActiveGameplayEffect(ActualHealCooldown));
				Test->TestFalse(TEXT("离线到期Heal族标签已清除"), ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));
				Mark(TEXT("loss-detached"));
				Stage = 4;
				return false;
			}

			if (Stage == 4)
			{
				if (!Has(TEXT("same-process-passed")) || PS->bConnected)
				{
					return false;
				}

				Test->TestEqual(TEXT("同进程恢复后再次退出仍同实例"), InventorySignature(Owner), SavedInventory);
				Test->TestEqual(TEXT("同进程恢复保持每个原来源及GE/Spec"), SourceSignature(Owner), SavedSources);
				Mark(TEXT("restart-go"));
				Stage = 5;
				return false;
			}

			if (Stage == 5)
			{
				if (!Has(TEXT("restart-passed")) || !PS->bConnected || !Has(TEXT("observer-offline")))
				{
					return false;
				}

				Test->TestTrue(TEXT("两次恢复同Hero同ASC"), Owner == SavedHero.Get() && ASC == SavedASC.Get());
				Test->TestEqual(TEXT("恢复来源不重复"), ASC->GetSourceCount(), BaseSources + 4);
				Test->TestEqual(TEXT("恢复经济版本未变"), PS->GetEconomy().Revision, SavedRevision);
				Test->TestEqual(TEXT("恢复库存完全同实例"), InventorySignature(Owner), SavedInventory);
				Test->TestEqual(TEXT("重启恢复保持每个原来源及GE/Spec"), SourceSignature(Owner), SavedSources);
				Test->TestNotNull(TEXT("进程重启保持原GE handle/时钟"), ASC->GetActiveGameplayEffect(LongCooldown));
				Test->TestFalse(TEXT("进程重启不重加已到期五秒Heal族"), ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));
				Mark(TEXT("server-passed"));
				return true;
			}

			return false;
		}

		bool Observe(AHeroCharacter* Owner)
		{
			if (Owner->GetAttributes()->GetShield() == 30 && Owner->GetAttributes()->GetHealth() == 65 && Owner->GetAttributes()->GetArmor() == 10)
			{
				Test->TestTrue(TEXT("观察端实际减伤复制"), FMath::IsNearlyEqual(Owner->GetAttributes()->GetDamageTakenMultiplier(), .9f));
				Test->TestTrue(TEXT("观察端不依赖私有库存"), Owner->GetInventory()->GetSnapshot().Items.IsEmpty());
				Mark(TEXT("observer-shield"));
			}

			if (Has(TEXT("dead-go")) && !Owner->IsAlive())
			{
				Test->TestEqual(TEXT("观察者死亡盾清零"), Owner->GetAttributes()->GetShield(), 0.f);
				Mark(TEXT("observer-dead"));
			}

			if (Has(TEXT("loss-start")) && FMath::IsNearlyEqual(Owner->GetAttributes()->GetHealth(), 86.5f))
			{
				Mark(TEXT("observer-offline"));
			}

			return false;
		}

		bool Initial(AHeroPlayerController* PC, AHeroCharacter* Hero)
		{
			if (!Has(TEXT("commands-go")) || !Coherent(PC, Hero))
			{
				return false;
			}

			auto* PS = PC->GetPlayerState<AParticipantState>();
			const auto& Last = PC->GetLastItemResult();
			if (Stage == 0)
			{
				PC->MobaBuy(TEXT("ArmorPlate"));
				Stage = 1;
				return false;
			}

			if (Stage >= 1 && Stage <= 10)
			{
				if (Last.Sequence != static_cast<uint32>(Stage))
				{
					return false;
				}

				if (!Last.bAccepted)
				{
					return Fail(TEXT("命令意外拒绝"));
				}

				if (Hero->GetInventory()->GetSnapshot().Revision != Last.Revision)
				{
					return false;
				}

				switch (Stage)
				{
				case 1:
					Test->TestEqual(TEXT("owner900金币复制"), PS->GetEconomy().Gold, 900);
					PC->MobaBuy(TEXT("FocusCharm"));
					break;
				case 2:
					PC->MobaCombine(TEXT("CompositeGuard"));
					break;
				case 3:
					Test->TestEqual(TEXT("配方只加100"), PS->GetEconomy().Gold, 650);
					PC->MobaBuy(TEXT("GuardOrb"), 2);
					break;
				case 4:
					PC->MobaBuy(TEXT("HealingPotion"), 3);
					break;
				case 5:
					Orb = First(Hero, TEXT("GuardOrb"));
					Potion = First(Hero, TEXT("HealingPotion"));
					PC->MobaUseItem(Orb.ToString());
					break;
				case 6:
					PC->MobaUseItem(Potion.ToString());
					break;
				case 7:
					if (Hero->GetAttributes()->GetHealth() != 65 || Hero->GetAttributes()->GetShield() != 30 || !Has(TEXT("observer-shield")))
					{
						return false;
					}

					Test->TestEqual(TEXT("真实治愈后药数量2"), Hero->GetInventory()->GetSnapshot().Items.Last().Quantity, 2);
					PC->MobaSell(Orb.ToString());
					break;
				case 8:
					PC->MobaBuy(TEXT("GuardOrb"));
					break;
				case 9:
					PC->MobaItems();
					SavedRevision = Last.Revision;
					Probe = Intent(Hero, 10);
					Probe.Operation = EItemOperation::Use;
					Probe.Instance = Hero->GetInventory()->GetSnapshot().Items.Last().Id;
					PC->ServerItemRequest(Probe);
					Stage = 20;
					return false;
				default:
					break;
				}

				++Stage;
				return false;
			}

			if (Stage == 20)
			{
				if (Last.Sequence != 10)
				{
					return false;
				}

				Test->TestEqual(TEXT("出售再买仍同5秒冷却拒绝"), Last.Reason, FName(TEXT("Cooldown")));
				Test->TestEqual(TEXT("冷却失败版本未变"), Hero->GetInventory()->GetSnapshot().Revision, SavedRevision);
				Probe = Intent(Hero, 11);
				Probe.Quantity = 0;
				PC->ServerItemRequest(Probe);
				Stage = 21;
				return false;
			}

			if (Stage == 21)
			{
				if (Last.Sequence != 11)
				{
					return false;
				}

				Test->TestEqual(TEXT("真实RPC非法数量拒绝"), Last.Reason, FName(TEXT("Quantity")));
				Probe = Intent(Hero, 12);
				FGuid::Parse(Read(TEXT("observer-identity")), Probe.Entity);
				PC->ServerItemRequest(Probe);
				Stage = 22;
				return false;
			}

			if (Stage == 22)
			{
				if (Last.Sequence != 12)
				{
					return false;
				}

				Test->TestEqual(TEXT("真实RPC越权Entity拒绝"), Last.Reason, FName(TEXT("Entity")));
				Probe = Intent(Hero, 13);
				--Probe.Epoch;
				PC->ServerItemRequest(Probe);
				Stage = 23;
				return false;
			}

			if (Stage == 23)
			{
				if (Last.Sequence != 13)
				{
					return false;
				}

				Test->TestEqual(TEXT("真实RPC旧Epoch拒绝"), Last.Reason, FName(TEXT("Epoch")));
				Probe = Intent(Hero, 14);
				--Probe.Revision;
				PC->ServerItemRequest(Probe);
				Stage = 24;
				return false;
			}

			if (Stage == 24)
			{
				if (Last.Sequence != 14)
				{
					return false;
				}

				Test->TestEqual(TEXT("真实RPC旧版本拒绝"), Last.Reason, FName(TEXT("InventoryRevision")));
				// 重放已成功的Potion请求，原序号7必须返回原revision而不再治疗/扣数量。
				Probe = Intent(Hero, 7);
				Probe.Operation = EItemOperation::Use;
				Probe.Instance = Potion;
				PC->ServerItemRequest(Probe);
				Stage = 25;
				return false;
			}

			if (Stage == 25)
			{
				if (Last.Sequence != 7)
				{
					return false;
				}

				Test->TestTrue(TEXT("重复序号返回原成功"), Last.bAccepted);
				Test->TestEqual(TEXT("重复结果原revision7"), Last.Revision, uint32(7));
				Test->TestEqual(TEXT("去重不改当前revision9"), Hero->GetInventory()->GetSnapshot().Revision, SavedRevision);
				Test->TestEqual(TEXT("去重HP未二次Heal"), Hero->GetAttributes()->GetHealth(), 65.f);
				Test->TestEqual(TEXT("最终Gold0复制"), PS->GetEconomy().Gold, 0);
				Mark(TEXT("commands-done"));
				Stage = 30;
				return false;
			}

			if (Stage == 30)
			{
				if (!Has(TEXT("dead-go")) || Hero->IsAlive())
				{
					return false;
				}

				Test->TestEqual(TEXT("owner死亡库存保持"), InventorySignature(Hero), Read(TEXT("saved-inventory")));
				Test->TestEqual(TEXT("owner死亡金币保持"), PS->GetEconomy().Gold, 0);
				Mark(TEXT("initial-dead"));
				Stage = 40;
				return false;
			}

			if (Stage == 40)
			{
				if (!Has(TEXT("loss-go")) || !Hero->IsAlive())
				{
					return false;
				}

				if (!UIPhaseReady(TEXT("ui-loss-seeded")))
				{
					return false;
				}

				SavedEpoch = Hero->ConnectionEpoch;
				Address = Hero->GetWorld()->URL.Host + TEXT(":") + FString::FromInt(Hero->GetWorld()->URL.Port);
				Test->AddExpectedError(TEXT("UEngine::BroadcastNetworkFailure: FailureType = ConnectionTimeout"), EAutomationExpectedErrorFlags::Contains, 1);
				FPacketSimulationSettings Loss;
				Loss.PktLoss = 100;
				Loss.PktIncomingLoss = 100;
				Hero->GetWorld()->GetNetDriver()->SetPacketSimulationSettings(Loss);
				Mark(TEXT("loss-start"));
				Stage = 41;
				return false;
			}

			return false;
		}

		bool Reconnected(AHeroPlayerController* PC, AHeroCharacter* Hero, bool bRestart)
		{
			if (!Coherent(PC, Hero) || !Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemShield)
				|| !FMath::IsNearlyEqual(Hero->GetAttributes()->GetHealth(), 86.5f))
			{
				return false;
			}

			Test->TestEqual(TEXT("恢复原Hero身份"), Hero->EntityId.ToString(), Read(TEXT("initial-identity")));
			Test->TestEqual(TEXT("恢复原金币不再发"), PC->GetPlayerState<AParticipantState>()->GetEconomy().Gold, 0);
			Test->TestEqual(TEXT("恢复全部实例/数量/Spec句柄"), InventorySignature(Hero), Read(TEXT("saved-inventory")));
			Test->TestEqual(TEXT("恢复唯一死亡代次"), Hero->GetLifeSnapshot().DeathCount, uint32(1));
			Test->TestFalse(TEXT("恢复读取真实五秒Heal冷却已经到期"), Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));
			if (!bRestart)
			{
				Test->TestTrue(TEXT("真实网络超时重连推进Epoch"), Hero->ConnectionEpoch > SavedEpoch);
			}

			const auto Times =
				Hero->GetHeroAbilitySystem()->GetActiveEffectsTimeRemaining(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_ItemShield)));
			Test->TestTrue(TEXT("原剩余时钟不重置180秒"), Times.Num() == 1 && Times[0] > 0 && Times[0] < 150);
			if (!UIPhaseReady(bRestart ? TEXT("ui-restart-restored") : TEXT("ui-same-process-restored")))
			{
				return false;
			}

			PC->MobaItems();
			Mark(bRestart ? TEXT("restart-passed") : TEXT("same-process-passed"));
			return bRestart ? Has(TEXT("server-passed")) : true;
		}

		FAutomationTestBase* Test;


	private:
		FString Role, Evidence, Address, SavedInventory, SavedSources;

		FString UIRunToken;

		double Started;

		int32 Stage = 0, BaseSources = 0, BaseSpecs = 0;

		bool bNoItems = false, bDotApplied = false;

		bool bUIRecovery = false;

		FGuid Orb, Potion;

		uint32 SavedRevision = 0, SavedEpoch = 0;

		FItemRequest Probe;

		TWeakObjectPtr<AParticipantState> SavedParticipant;

		TWeakObjectPtr<AHeroCharacter> SavedHero;

		TWeakObjectPtr<UHeroAbilitySystemComponent> SavedASC;

		FActiveGameplayEffectHandle LongCooldown, OfflineDot, ActualHealCooldown;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemNetworkTest, "Moba.Network.Items", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FItemNetworkTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FItemNetworkScenario(this));
	return true;
}
#endif
