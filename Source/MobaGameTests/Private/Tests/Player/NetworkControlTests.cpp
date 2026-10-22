#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "EnhancedPlayerInput.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputKeyEventArgs.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "MobaGameplayTags.h"

namespace
{
	class FNetworkControlScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FNetworkControlScenario(FAutomationTestBase* InTest) : Test(InTest)
		{
			bDriver = FParse::Param(FCommandLine::Get(), TEXT("MobaTestDriver"));
			Started = FPlatformTime::Seconds();
		}


	public:
		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 180)
			{
				Test->AddError(FString::Printf(TEXT("联机场景超时 Driver=%d Stage=%d"), bDriver, Stage));
				return true;
			}

			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->GetNetMode() == NM_Client && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			AHeroPlayerController* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			if (!Controller || !Controller->IsLocallyReady())
			{
				return false;
			}

			AHeroCharacter* LocalHero = Cast<AHeroCharacter>(Controller->GetPawn());
			AHeroCharacter* Other = nullptr;
			int32 Count = 0;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				++Count;
				if (*It != LocalHero)
				{
					Other = *It;
				}
			}

			if (!Other || !Other->EntityId.IsValid())
			{
				return false;
			}

			AParticipantState* OtherParticipant = nullptr;
			for (TActorIterator<AParticipantState> It(World); It; ++It)
			{
				if (It->ParticipantId == Other->ParticipantId)
				{
					OtherParticipant = *It;
					break;
				}
			}

			if (!OtherParticipant)
			{
				return false;
			}

			if (Stage == 0)
			{
				if (!Other->bInputReady)
				{
					return false;
				}

				Test->TestEqual(TEXT("两个玩家只有两个 Hero"), Count, 2);
				Test->TestTrue(TEXT("不同参赛者身份"), LocalHero->ParticipantId != Other->ParticipantId);
				Test->TestTrue(TEXT("不同实体身份"), LocalHero->EntityId != Other->EntityId);
				// 与 L_FrameworkTest 的两处出生配置对应，防止误选宿主 Entry 中的出生点。
				const auto IsAtTestMapStart = [](const FVector& Position)
				{ return FMath::IsNearlyEqual(Position.X, -500.0, 5.0) && FMath::IsNearlyEqual(FMath::Abs(Position.Y), 300.0, 5.0); };
				Test->TestTrue(TEXT("本玩家使用内容地图出生点"), IsAtTestMapStart(LocalHero->GetActorLocation()));
				Test->TestTrue(TEXT("另一玩家使用内容地图出生点"), IsAtTestMapStart(Other->GetActorLocation()));
				Test->TestTrue(TEXT("两名玩家占用不同出生点"), FMath::Abs(LocalHero->GetActorLocation().Y - Other->GetActorLocation().Y) > 500.0);
				InitialPosition = bDriver ? LocalHero->GetActorLocation() : Other->GetActorLocation();
				ObservedEntity = Other->EntityId;
				ObservedEpoch = Other->ConnectionEpoch;
				ObservedActor = Other;
				ObservedParticipant = OtherParticipant;
				ObservedASC = Other->GetHeroAbilitySystem();
				++Stage;
				Controller->MobaStatus();
				if (!bDriver)
				{
					Test->AddInfo(TEXT("ObserverReady：开始观察独立客户端的复制后果。"));
				}

				return false;
			}

			if (!bDriver)
			{
				bObservedMovement |= FVector::Dist2D(InitialPosition, Other->GetActorLocation()) >= 150;
				bObservedClickMovement |= FMath::Abs(Other->GetActorLocation().Y - InitialPosition.Y) >= 150;
				bObservedCost |= Other->GetAttributes()->GetMana() < 100;
				bObservedCooldown |= Other->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self);
				if (OtherParticipant->bConnected)
				{
					return false;
				}

				if (Other->ConnectionEpoch <= ObservedEpoch || Other->bInputReady)
				{
					return false;
				}

				if (Other->GetAttributes()->GetMana() != 0.0f)
				{
					return false;
				}

				if (!Other->GetVelocity().IsNearlyZero(1.0f))
				{
					return false;
				}

				Test->TestTrue(TEXT("另一客户端观察到移动复制"), bObservedMovement);
				Test->TestTrue(TEXT("另一客户端观察到Click阶段的横向移动复制"), bObservedClickMovement);
				Test->TestTrue(TEXT("另一客户端观察到成本复制"), bObservedCost);
				Test->TestTrue(TEXT("另一客户端观察到冷却标签复制"), bObservedCooldown);
				Test->TestEqual(TEXT("断线保留同一个 Hero"), Other->EntityId, ObservedEntity);
				Test->TestTrue(TEXT("原 Participant 仍引用同一 Hero"), OtherParticipant->PrimaryHero == Other);
				Test->TestTrue(TEXT("断线推进旧连接代次"), Other->ConnectionEpoch > ObservedEpoch);
				Test->TestFalse(TEXT("断线输入关闭"), Other->bInputReady);
				Test->TestTrue(TEXT("持续移动输入在断线后停止"), Other->GetVelocity().IsNearlyZero(1.0f));
				Test->TestEqual(TEXT("初始化代次仍为一"), Other->InitializationGeneration, uint32(1));
				Test->TestEqual(TEXT("断线资源没有重置"), Other->GetAttributes()->GetMana(), 0.0f);
				Test->TestNotNull(TEXT("原 ASC 仍在"), Other->GetAbilitySystemComponent());
				Test->TestTrue(TEXT("保留原Actor/PlayerState/ASC对象"),
					ObservedActor.Get() == Other && ObservedParticipant.Get() == OtherParticipant && ObservedASC.Get() == Other->GetHeroAbilitySystem());
				Controller->MobaStatus();
				Test->AddInfo(TEXT("Observer完成：另一玩家进程退出后，本客户端及服务器继续运行，原状态可复制。"));
				return true;
			}

			const UExperienceDefinition* Experience = World->GetSubsystem<UExperienceSubsystem>()->GetExperience();
			const UInputProfileDefinition* WASD = nullptr;
			const UInputProfileDefinition* Click = nullptr;
			for (const auto& Profile : Experience->InputProfiles)
			{
				if (Profile->Profile == EHeroInputProfile::WASD)
				{
					WASD = Profile.Get();
				}
				else
				{
					Click = Profile.Get();
				}
			}

			UEnhancedPlayerInput* Input = Cast<UEnhancedPlayerInput>(Controller->PlayerInput);
			if (!Input || !WASD || !Click)
			{
				Test->AddError(TEXT("真实 EnhancedInput/Profile 不可用"));
				return true;
			}

			switch (Stage)
			{
			case 1:
				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(Other->EntityId, LocalHero->ConnectionEpoch, LocalHero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_Primary);
				++Stage;
				return false;
			case 2:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestFalse(TEXT("服务器拒绝他人实体"), Controller->WasLastIntentAccepted());
				Test->TestEqual(TEXT("越权分类"), Controller->GetLastIntentReason(), FName(TEXT("Entity")));
				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(LocalHero->EntityId, LocalHero->ConnectionEpoch - 1, LocalHero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_Primary);
				++Stage;
				return false;
			case 3:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("旧代次拒绝"), Controller->GetLastIntentReason(), FName(TEXT("Epoch")));
				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateSlot(LocalHero->EntityId, LocalHero->ConnectionEpoch, LocalHero->RequiredLoadoutRevision + 1, MobaGameplayTags::Input_Slot_Primary);
				++Stage;
				return false;
			case 4:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestEqual(TEXT("旧/未知槽版本拒绝"), Controller->GetLastIntentReason(), FName(TEXT("Revision")));
				Test->TestEqual(TEXT("无效意图不扣自己资源"), LocalHero->GetAttributes()->GetMana(), 100.0f);
				Test->TestEqual(TEXT("无效意图不扣他人资源"), Other->GetAttributes()->GetMana(), 100.0f);
				Controller->ServerPlayerReady(LocalHero->EntityId, LocalHero->ConnectionEpoch, LocalHero->RequiredLoadoutRevision);
				Controller->ServerPlayerReady(LocalHero->EntityId, LocalHero->ConnectionEpoch, LocalHero->RequiredLoadoutRevision);
				++Stage;
				return false;
			case 5:
				if (FVector::Dist2D(InitialPosition, LocalHero->GetActorLocation()) < 250)
				{
					if (!bMoveKeyPressed)
					{
						Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Pressed, FPlatformTime::Cycles64()));
						bMoveKeyPressed = true;
					}

					return false;
				}

				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Released, FPlatformTime::Cycles64()));
				Input->InjectInputForAction(WASD->SwitchProfile.Get(), FInputActionValue(true));
				++Stage;
				return false;
			case 6:
				if (Controller->GetActiveProfile() != EHeroInputProfile::ClickToMove || !Controller->IsLocallyReady())
				{
					return false;
				}

				{
					UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
					Test->TestFalse(TEXT("切换后旧WASD映射被卸载"), Subsystem->HasMappingContext(WASD->Mapping.Get()));
					Test->TestTrue(TEXT("Click实际映射已激活"), Subsystem->HasMappingContext(Click->Mapping.Get()));
				}

				ClickStart = LocalHero->GetActorLocation();
				Controller->MobaClick(ClickStart.X, ClickStart.Y + 300, 0);
				Ack = Controller->GetIntentResultSequence();
				++Stage;
				return false;
			case 7:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Controller->WasLastIntentAccepted())
				{
					Test->AddError(TEXT("服务器导航点击拒绝"));
					return true;
				}

				if (FVector::Dist2D(ClickStart, LocalHero->GetActorLocation()) < 200)
				{
					return false;
				}

				Controller->MobaStatus();
				Ack = Controller->GetIntentResultSequence();
				// 选择限幅内的不同值，确保不是被本地clamp掩盖的无效篡改；服务器仍应从100扣至80。
				LocalHero->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 40);
				Test->TestEqual(TEXT("确实改变了客户端本地资源副本"), LocalHero->GetAttributes()->GetMana(), 40.0f);
				Input->InjectInputForAction(Click->PrimaryAbility.Get(), FInputActionValue(true));
				++Stage;
				return false;
			case 8:
				if (Controller->GetIntentResultSequence() == Ack || LocalHero->GetAttributes()->GetMana() != 80
					|| !LocalHero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
				{
					return false;
				}

				Test->TestEqual(TEXT("客户端资源篡改不能覆盖服务器状态"), LocalHero->GetAttributes()->GetMana(), 80.0f);
				Test->TestTrue(TEXT("稳定槽经服务器提交"), Controller->WasLastIntentAccepted());
				Test->TestTrue(TEXT("真实GAS冷却已复制"), LocalHero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self));
				Ack = Controller->GetIntentResultSequence();
				Controller->MobaAbility();
				++Stage;
				return false;
			case 9:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestFalse(TEXT("冷却内重复激活拒绝"), Controller->WasLastIntentAccepted());
				Test->TestEqual(TEXT("冷却拒绝分类"), Controller->GetLastIntentReason(), FName(TEXT("Cooldown")));
				Test->TestEqual(TEXT("冷却拒绝不重复扣资源"), LocalHero->GetAttributes()->GetMana(), 80.0f);
				RemainingCasts = 4;
				++Stage;
				return false;
			case 10:
				if (LocalHero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Controller->MobaAbility();
				++Stage;
				return false;
			case 11:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				if (!Controller->WasLastIntentAccepted())
				{
					Test->AddError(TEXT("冷却结束后合法能力未提交"));
					return true;
				}

				if (LocalHero->GetAttributes()->GetMana() != 20.0f * (RemainingCasts - 1))
				{
					return false;
				}

				--RemainingCasts;
				Stage = RemainingCasts > 0 ? 10 : 12;
				return false;
			case 12:
				if (LocalHero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Controller->MobaAbility();
				++Stage;
				return false;
			case 13:
				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestFalse(TEXT("无资源激活拒绝"), Controller->WasLastIntentAccepted());
				Test->TestEqual(TEXT("资源不足拒绝分类"), Controller->GetLastIntentReason(), FName(TEXT("Cost")));
				Test->TestEqual(TEXT("资源不为负"), LocalHero->GetAttributes()->GetMana(), 0.0f);
				Test->TestEqual(TEXT("重复ready没有再次初始化"), LocalHero->InitializationGeneration, uint32(1));
				Test->TestEqual(TEXT("拥有端初始两槽只授予一次"), LocalHero->GetHeroAbilitySystem()->GetActivatableAbilities().Num(), 2);
				Controller->MobaStatus();
				Controller->MobaProfile();
				++Stage;
				return false;
			case 14:
				if (Controller->GetActiveProfile() != EHeroInputProfile::WASD || !Controller->IsLocallyReady())
				{
					return false;
				}

				DisconnectStart = LocalHero->GetActorLocation();
				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::W, IE_Pressed, FPlatformTime::Cycles64()));
				++Stage;
				return false;
			case 15:
				if (FVector::Dist2D(DisconnectStart, LocalHero->GetActorLocation()) < 50)
				{
					return false;
				}

				Test->TestTrue(TEXT("客户端退出前仍有持续移动输入"), LocalHero->GetVelocity().Size2D() > 1);
				Test->AddInfo(TEXT("Driver完成；测试队列退出将终止本客户端，Observer继续验证断线。"));
				return true;
			}

			return false;
		}


	private:
		FAutomationTestBase* Test;

		bool bDriver = false;

		double Started = 0;

		int32 Stage = 0;

		int32 RemainingCasts = 0;

		uint32 Ack = 0;

		uint32 ObservedEpoch = 0;

		FGuid ObservedEntity;

		TWeakObjectPtr<AHeroCharacter> ObservedActor;

		TWeakObjectPtr<AParticipantState> ObservedParticipant;

		TWeakObjectPtr<UHeroAbilitySystemComponent> ObservedASC;

		FVector InitialPosition = FVector::ZeroVector;

		FVector ClickStart = FVector::ZeroVector;

		FVector DisconnectStart = FVector::ZeroVector;

		bool bObservedMovement = false;

		bool bObservedClickMovement = false;

		bool bObservedCost = false;

		bool bObservedCooldown = false;

		bool bMoveKeyPressed = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkControlTest, "Moba.Network.Control", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FNetworkControlTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNetworkControlScenario>(this));
	return true;
}

#endif
