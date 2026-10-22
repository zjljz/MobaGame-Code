#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "InputKeyEventArgs.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"

namespace
{
	class FNetworkCombatScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FNetworkCombatScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			bDriver = FParse::Param(FCommandLine::Get(), TEXT("MobaTestDriver"));
		}


	private:
		// 测试攻击与拒绝探测
		void Attack(AHeroPlayerController* Controller, AHeroCharacter* Source, AHeroCharacter* Target)
		{
			if (bDriver && Stage == 6 && FParse::Param(FCommandLine::Get(), TEXT("MobaRenderedInput")))
			{
				FVector2D Screen;
				if (!Controller->ProjectWorldLocationToScreen(Target->GetActorLocation(), Screen))
				{
					Test->AddError(TEXT("无法把目标投影到真实视口"));
					return;
				}

				Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Pressed, FPlatformTime::Cycles64()));
				bAttackKeyHeld = true;
				Test->AddInfo(FString::Printf(TEXT("RenderedInput E + cursor target at %.0f,%.0f"), Screen.X, Screen.Y));
				return;
			}

			Controller->ServerActivateUnitSlot(Source->EntityId, Source->ConnectionEpoch, Source->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack, Target);
		}

		void SendReject(AHeroPlayerController* Controller, AHeroCharacter* Source, AHeroCharacter* Target, int32 Index)
		{
			Ack = Controller->GetIntentResultSequence();
			Controller->ServerActivateUnitSlot(Index == 0 ? Target->EntityId : Source->EntityId, Source->ConnectionEpoch - (Index == 1 ? 1 : 0),
				Source->RequiredLoadoutRevision + (Index == 2 ? 1 : 0), Index == 3 ? MobaGameplayTags::Input_Slot_Primary : MobaGameplayTags::Input_Slot_UnitAttack,
				Index == 4		 ? nullptr
				: Index == 5 ? Source
				: Target);
		}


		// 身份与截图检查
		void CheckIdentity(AHeroCharacter* Victim)
		{
			Test->TestEqual(TEXT("原实体身份"), Victim->EntityId, Entity);
			Test->TestTrue(TEXT("原Hero/ASC对象"), OriginalHero.Get() == Victim && OriginalASC.Get() == Victim->GetHeroAbilitySystem());
			Test->TestEqual(TEXT("初始化仅一次"), Victim->InitializationGeneration, uint32(1));
		}

		void Capture(const TCHAR* Phase)
		{
			FString Directory;
			if (FParse::Param(FCommandLine::Get(), TEXT("MobaRenderedInput")) && FParse::Value(FCommandLine::Get(), TEXT("MobaScreenshotDirectory="), Directory))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString(Phase) + TEXT(".png")), false, false);
			}
		}


	public:
		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 150)
			{
				Test->AddError(FString::Printf(TEXT("Combat超时 Driver=%d Stage=%d"), bDriver, Stage));
				return true;
			}

			UWorld* World = nullptr;
			for (const auto& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->GetNetMode() == NM_Client)
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			auto* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Local = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
			if (!Local || !Local->EntityId.IsValid())
			{
				return false;
			}

			if (bAttackKeyHeld)
			{
				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Released, FPlatformTime::Cycles64()));
				bAttackKeyHeld = false;
			}

			AHeroCharacter* Other = nullptr;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (*It != Local)
				{
					Other = *It;
					break;
				}
			}

			if (!Other || !Other->EntityId.IsValid())
			{
				return false;
			}

			AHeroCharacter* Victim = bDriver ? Other : Local;
			AHeroCharacter* Attacker = bDriver ? Local : Other;
			auto* Abilities = Local->GetHeroAbilitySystem();
			auto* VictimASC = Victim->GetHeroAbilitySystem();
			const FHeroLifeSnapshot& Life = Victim->GetLifeSnapshot();
			if (Stage == 0)
			{
				if (!Controller->IsLocallyReady() || !Other->bInputReady || !VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Effect_Persistent)
					|| !VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Effect_RemoveOnDeath))
				{
					return false;
				}

				Entity = Victim->EntityId;
				OriginalHero = Victim;
				OriginalASC = VictimASC;
				InitialPosition = Victim->GetActorLocation();
				AttackStart = Attacker->GetActorLocation();
				Revision = Local->RequiredLoadoutRevision;
				AttackHandle = Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack);
				Test->TestTrue(TEXT("双方来自同一个Match"), Local->MatchId.IsValid() && Local->MatchId == Other->MatchId);
				Test->TestEqual(TEXT("两个稳定槽"), Abilities->GetActivatableAbilities().Num(), 2);
				Stage = 1;
				Controller->MobaStatus();
				Capture(TEXT("alive"));
				if (!bDriver)
				{
					const auto Handles = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Persistent)));
					if (Handles.Num() != 1)
					{
						Test->AddError(TEXT("拥有端持久GE数量错误"));
						return true;
					}

					PersistentHandle = Handles[0];
					PersistentStart = Abilities->GetActiveGameplayEffect(PersistentHandle)->StartServerWorldTime;
				}

				return false;
			}

			if (!bDriver)
			{
				if (Stage == 1)
				{
					// Driver 用自己的 Self 冷却作为准备信号；Victim 此时提交自身冷却，再接受攻击。
					if (!Attacker->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
					{
						return false;
					}

					Controller->MobaAbility();
					Stage = 2;
					return false;
				}

				if (Stage == 2)
				{
					bSawDamage |= Local->GetAttributes()->GetHealth() == 40;
					if (Life.State != EHeroLifeState::Dead || Local->GetAttributes()->GetHealth() != 0 || Abilities->HasMatchingGameplayTag(MobaGameplayTags::Effect_RemoveOnDeath))
					{
						return false;
					}

					Test->TestTrue(TEXT("拥有端见第一击40HP"), bSawDamage);
					Test->TestEqual(TEXT("第一次死亡归属"), Life.KillerEntity, Other->EntityId);
					Test->TestEqual(TEXT("第一次唯一死亡"), Life.DeathCount, uint32(1));
					Test->TestFalse(TEXT("死者本地输入门关闭"), Controller->IsLocallyReady());
					Test->TestTrue(TEXT("死者移动关闭"), Local->GetCharacterMovement()->MovementMode == MOVE_None);
					Test->TestTrue(TEXT("死者碰撞关闭"), Local->GetCapsuleComponent()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
					Test->TestTrue(TEXT("死者自己的冷却继续"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self));
					Test->TestTrue(TEXT("死者持久GE继续"), Abilities->GetActiveGameplayEffect(PersistentHandle) != nullptr);
					Capture(TEXT("dead"));
					DeadPosition = Local->GetActorLocation();
					Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Pressed, FPlatformTime::Cycles64()));
					Ack = Controller->GetIntentResultSequence();
					Attack(Controller, Local, Other);
					Stage = 3;
					return false;
				}

				if (Stage == 3)
				{
					if (Controller->GetIntentResultSequence() == Ack)
					{
						return false;
					}

					Test->TestFalse(TEXT("死者RPC被服务器拒绝"), Controller->WasLastIntentAccepted());
					Test->TestEqual(TEXT("死亡拒绝原因"), Controller->GetLastIntentReason(), FName(TEXT("LifeState")));
					Test->TestTrue(TEXT("死者W输入未移动"), FVector::Dist(Local->GetActorLocation(), DeadPosition) < 1);
					Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Released, FPlatformTime::Cycles64()));
					Stage = 4;
					return false;
				}

				if (Stage == 4)
				{
					if (!Local->IsAlive() || Life.Generation != 2 || Local->GetAttributes()->GetHealth() != 100 || !Controller->IsLocallyReady())
					{
						return false;
					}

					CheckIdentity(Victim);
					Test->TestTrue(TEXT("拥有端原SlotHandle"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack) == AttackHandle);
					Test->TestEqual(TEXT("拥有端revision未重授"), Local->RequiredLoadoutRevision, Revision);
					const auto* Persistent = Abilities->GetActiveGameplayEffect(PersistentHandle);
					Test->TestNotNull(TEXT("复活原持久GE handle"), Persistent);
					if (Persistent)
					{
						Test->TestEqual(TEXT("持久GE服务器开始时间不重置"), Persistent->StartServerWorldTime, PersistentStart);
					}

					Test->TestFalse(TEXT("死亡清理GE未被重新grant"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Effect_RemoveOnDeath));
					Test->TestTrue(TEXT("复活碰撞恢复"), Local->GetCapsuleComponent()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
					Capture(TEXT("respawn"));
					// 以实际第二次移动和返回为控制恢复证明，完成后再用Self效果通知Driver。
					MoveStart = Local->GetActorLocation();
					Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Pressed, FPlatformTime::Cycles64()));
					Stage = 5;
					return false;
				}

				if (Stage == 5)
				{
					if (FVector::Dist2D(Local->GetActorLocation(), MoveStart) < 100)
					{
						return false;
					}

					Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Released, FPlatformTime::Cycles64()));
					if (Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
					{
						return false;
					}

					Controller->MobaAbility();
					Stage = 6;
					return false;
				}

				if (Stage == 6)
				{
					if (Life.State != EHeroLifeState::Dead || Life.DeathCount != 2 || Local->GetAttributes()->GetHealth() != 0)
					{
						return false;
					}

					CheckIdentity(Victim);
					Test->TestEqual(TEXT("第二生命仍只有一次结算"), Life.DeathCount, uint32(2));
					Test->TestTrue(TEXT("退出前仍在持续计时"), Abilities->GetActiveGameplayEffect(PersistentHandle) != nullptr);
					Controller->MobaStatus();
					Test->AddInfo(TEXT("Observer在第二次死亡期间正常结束客户端；Driver继续核对服务器断线复活。"));
					return true;
				}

				return false;
			}

			switch (Stage)
			{
			case 1:
				SendReject(Controller, Local, Other, 0);
				Stage = 2;
				return false;
			case 2:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				{
					const FName Reasons[] = {TEXT("Entity"), TEXT("Epoch"), TEXT("Revision"), TEXT("Slot"), TEXT("Target"), TEXT("Target")};
					Test->TestFalse(TEXT("无效意图拒绝"), Controller->WasLastIntentAccepted());
					Test->TestEqual(TEXT("拒绝分类"), Controller->GetLastIntentReason(), Reasons[RejectIndex]);
					Test->TestEqual(TEXT("拒绝不半扣费"), Local->GetAttributes()->GetMana(), 100.0f);
					Test->TestEqual(TEXT("拒绝不造成伤害"), Victim->GetAttributes()->GetHealth(), 100.0f);
					if (++RejectIndex < 6)
					{
						SendReject(Controller, Local, Other, RejectIndex);
						return false;
					}
				}

				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Pressed, FPlatformTime::Cycles64()));
				Stage = 3;
				return false;
			case 3:
				if (FVector::Dist(Local->GetActorLocation(), Victim->GetActorLocation()) < 1050)
				{
					return false;
				}

				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Released, FPlatformTime::Cycles64()));
				Ack = Controller->GetIntentResultSequence();
				Attack(Controller, Local, Other);
				Stage = 4;
				return false;
			case 4:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("真实越距拒绝"), Controller->GetLastIntentReason(), FName(TEXT("Range")));
				Test->TestEqual(TEXT("越距无扣费"), Local->GetAttributes()->GetMana(), 100.0f);
				Test->TestEqual(TEXT("越距无伤害"), Victim->GetAttributes()->GetHealth(), 100.0f);
				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::S, IE_Pressed, FPlatformTime::Cycles64()));
				Stage = 5;
				return false;
			case 5:
				if (FVector::Dist2D(Local->GetActorLocation(), AttackStart) > 50)
				{
					return false;
				}

				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::S, IE_Released, FPlatformTime::Cycles64()));
				Controller->MobaAbility();
				Stage = 6;
				return false;
			case 6:
				if (!VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self) || Local->GetAttributes()->GetMana() != 80)
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Attack(Controller, Local, Other);
				Stage = 7;
				return false;
			case 7:
				if (Controller->GetIntentResultSequence() == Ack || Victim->GetAttributes()->GetHealth() != 40 || Local->GetAttributes()->GetMana() != 60)
				{
					return false;
				}

				Test->TestTrue(TEXT("第一击成功"), Controller->WasLastIntentAccepted());
				Ack = Controller->GetIntentResultSequence();
				Attack(Controller, Local, Other);
				Stage = 8;
				return false;
			case 8:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("冷却拒绝"), Controller->GetLastIntentReason(), FName(TEXT("Cooldown")));
				Test->TestEqual(TEXT("冷却无额外伤害"), Victim->GetAttributes()->GetHealth(), 40.0f);
				Test->TestEqual(TEXT("冷却无额外成本"), Local->GetAttributes()->GetMana(), 60.0f);
				Stage = 9;
				return false;
			case 9:
				if (Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Attack(Controller, Local, Other);
				Stage = 10;
				return false;
			case 10:
				if (Life.State != EHeroLifeState::Dead || Victim->GetAttributes()->GetHealth() != 0 || Local->GetAttributes()->GetMana() != 40)
				{
					return false;
				}

				CheckIdentity(Victim);
				Test->TestEqual(TEXT("远端死亡唯一归属"), Life.KillerParticipant, Local->ParticipantId);
				Test->TestTrue(TEXT("远端冷却跨死亡"), VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self));
				Test->TestTrue(TEXT("远端持久标签跨死亡"), VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Effect_Persistent));
				Ack = Controller->GetIntentResultSequence();
				Attack(Controller, Local, Other);
				Stage = 11;
				return false;
			case 11:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("继续攻击死者拒绝"), Controller->GetLastIntentReason(), FName(TEXT("LifeState")));
				Test->TestEqual(TEXT("死者不重复结算"), Life.DeathCount, uint32(1));
				Test->TestEqual(TEXT("死者攻击无扣费"), Local->GetAttributes()->GetMana(), 40.0f);
				Stage = 12;
				return false;
			case 12:
				if (!Victim->IsAlive() || Life.Generation != 2 || !VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self) || Victim->GetAttributes()->GetMana() != 80
					|| FVector::Dist2D(Victim->GetActorLocation(), InitialPosition) < 75)
				{
					return false;
				}

				Test->TestEqual(TEXT("复活100HP"), Victim->GetAttributes()->GetHealth(), 100.0f);
				CheckIdentity(Victim);
				Attack(Controller, Local, Other);
				Stage = 13;
				return false;
			case 13:
				if (Victim->GetAttributes()->GetHealth() != 40 || Local->GetAttributes()->GetMana() != 20 || Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Attack(Controller, Local, Other);
				Stage = 14;
				return false;
			case 14:
				if (Life.DeathCount != 2 || Life.State != EHeroLifeState::Dead || Local->GetAttributes()->GetMana() != 0)
				{
					return false;
				}

				bSawSecondDeath = true;
				Stage = 15;
				return false;
			case 15:
			{
				AParticipantState* Participant = nullptr;
				for (TActorIterator<AParticipantState> It(World); It; ++It)
				{
					if (It->ParticipantId == Victim->ParticipantId)
					{
						Participant = *It;
						break;
					}
				}

				if (!Participant || Participant->bConnected || !Victim->IsAlive() || Life.Generation != 3 || Victim->GetAttributes()->GetHealth() != 100)
				{
					return false;
				}

				Test->TestTrue(TEXT("实际观察第二次死亡"), bSawSecondDeath);
				Test->TestFalse(TEXT("断线复活不打开输入"), Victim->bInputReady);
				Test->TestTrue(TEXT("断线复活仍由原PS持有"), Participant->PrimaryHero == Victim);
				Test->TestTrue(TEXT("断线持续GE不消失"), VictimASC->HasMatchingGameplayTag(MobaGameplayTags::Effect_Persistent));
				CheckIdentity(Victim);
				if (Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Attack(Controller, Local, Other);
				Stage = 16;
				return false;
			}
			case 16:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("资源不足拒绝"), Controller->GetLastIntentReason(), FName(TEXT("Cost")));
				Test->TestEqual(TEXT("资源不足没有伤害"), Victim->GetAttributes()->GetHealth(), 100.0f);
				Test->TestEqual(TEXT("资源不为负"), Local->GetAttributes()->GetMana(), 0.0f);
				Test->TestEqual(TEXT("断线第二次复活只有两次死亡"), Life.DeathCount, uint32(2));
				Test->TestEqual(TEXT("本地能力仍只授予两项"), Abilities->GetActivatableAbilities().Num(), 2);
				Controller->MobaStatus();
				Test->AddInfo(TEXT("Driver完成：独立Server继续模拟已退出Observer的原Hero，完成第二次复活。"));
				return true;
			}

			return false;
		}


	private:
		FAutomationTestBase* Test;

		double Started;

		bool bDriver = false;

		bool bSawDamage = false;

		bool bSawSecondDeath = false;

		bool bAttackKeyHeld = false;

		int32 Stage = 0;

		int32 RejectIndex = 0;

		uint32 Ack = 0;

		uint32 Revision = 0;

		FGuid Entity;

		TWeakObjectPtr<AHeroCharacter> OriginalHero;

		TWeakObjectPtr<UHeroAbilitySystemComponent> OriginalASC;

		FGameplayAbilitySpecHandle AttackHandle;

		FActiveGameplayEffectHandle PersistentHandle;

		float PersistentStart = 0;

		FVector InitialPosition;

		FVector AttackStart;

		FVector DeadPosition;

		FVector MoveStart;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkCombatTest, "Moba.Network.Combat", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FNetworkCombatTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNetworkCombatScenario>(this));
	return true;
}

#endif
