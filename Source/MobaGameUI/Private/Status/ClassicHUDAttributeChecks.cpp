#include "Hero/HeroAttributeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Blueprint/UserWidget.h"
#include "ClientPresentationSubsystem.h"
#include "Combat/HeroAttributeSet.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "Match/ParticipantState.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Player/HeroPlayerController.h"
#include "Status/ClassicHUDWidget.h"
#include "Widgets/SViewport.h"

namespace
{
	bool TestValue(FAutomationTestBase* Test, const TCHAR* Label, float Actual, float Expected)
	{
		return Test->TestTrue(FString::Printf(TEXT("%s：实际 %.6f，预期 %.6f"), Label, Actual, Expected), FMath::IsNearlyEqual(Actual, Expected, .001f));
	}
}

namespace
{
	class FHeroAttributeLiveLifecycle final : public IAutomationLatentCommand
	{
	public:
		explicit FHeroAttributeLiveLifecycle(FAutomationTestBase* InTest) : Test(InTest), ReadyDeadline(FPlatformTime::Seconds() + 20)
		{
		}

		~FHeroAttributeLiveLifecycle()
		{
			if (GEditor && GEditor->PlayWorld)
			{
				GEditor->RequestEndPlayMap();
			}
		}

		virtual bool Update() override
		{
			if (!Hero.IsValid())
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* World = Context.World();
					auto* Controller = World && World->WorldType == EWorldType::PIE ? Cast<AHeroPlayerController>(World->GetFirstPlayerController()) : nullptr;
					if (World && World->WorldType == EWorldType::PIE && World->GetFirstPlayerController())
					{
						auto* LocalPlayer = World->GetFirstPlayerController()->GetLocalPlayer();
						auto* Presentation = LocalPlayer ? LocalPlayer->GetSubsystem<UClientPresentationSubsystem>() : nullptr;
						if (auto* Frontend = Presentation ? Presentation->FindLayer(TEXT("Frontend")) : nullptr)
						{
							Frontend->ProcessEvent(Frontend->FindFunctionChecked(TEXT("StartLocalMatch")), nullptr);
						}
					}

					auto* Candidate = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
					if (!Candidate || !Candidate->HasAuthority() || !Candidate->CanAcceptPlayerInput())
					{
						continue;
					}

					auto* LocalPlayer = Controller->GetLocalPlayer();
					auto* Presentation = LocalPlayer ? LocalPlayer->GetSubsystem<UClientPresentationSubsystem>() : nullptr;
					auto* CandidateHUD = Presentation ? Cast<UClassicHUDWidget>(Presentation->FindLayer(TEXT("Status"))) : nullptr;
					if (!CandidateHUD || !CandidateHUD->GetPresentedStatus().bProgressionReady)
					{
						continue;
					}

					Hero = Candidate;
					HUD = CandidateHUD;
					AbilitySystem = Candidate->GetHeroAbilitySystem();
					Generation = Candidate->GetLifeSnapshot().Generation;
					Test->TestEqual(TEXT("真实PIE首次HUD等级已经显示"), CandidateHUD->GetPresentedStatus().HeroLevel, 1);
					Test->TestEqual(TEXT("真实PIE首次HUD经验已经显示"), CandidateHUD->GetPresentedStatus().TotalExperiencePoints, int64(0));
					const int32 AbilityCount = AbilitySystem->GetActivatableAbilities().Num();
					const auto* Participant = Controller->GetPlayerState<AParticipantState>();
					Candidate->GetAttributeComponent()->FlushRegeneration();
					AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 50);
					Test->TestTrue(TEXT("真实开发Hero重复初始化幂等"), Candidate->InitializeHero(Candidate->GetDefinition(), Participant));
					TestValue(Test, TEXT("真实重复初始化不补血"), Candidate->GetAttributes()->GetHealth(), 50);
					Test->TestEqual(TEXT("真实能力没有重复授予"), AbilitySystem->GetActivatableAbilities().Num(), AbilityCount);
					int32 AttributeSetCount = 0;
					for (const auto* Set : AbilitySystem->GetSpawnedAttributes())
					{
						AttributeSetCount += Set == Candidate->GetAttributes() ? 1 : 0;
					}

					Test->TestEqual(TEXT("同一Set只注册一次"), AttributeSetCount, 1);
					const float ManaBefore = Candidate->GetAttributes()->GetMana();
					Test->TestTrue(TEXT("真实主槽可以经原生GAS激活"), AbilitySystem->TryActivateAbility(AbilitySystem->FindSlot(MobaGameplayTags::Input_Slot_Primary)));
					TestValue(Test, TEXT("开发技能Mana成本只扣20一次"), Candidate->GetAttributes()->GetMana(), ManaBefore - 20);
					const auto Result = Candidate->GetAttributeComponent()->AddExperience(350);
					Test->TestTrue(TEXT("真实开发经验入口接受350"), Result.bAccepted);
					TestValue(Test, TEXT("真实跨级到三级"), Candidate->GetAttributes()->GetHeroLevel(), 3);
					TestValue(Test, TEXT("真实跨级生命保持比例"), Candidate->GetAttributes()->GetHealth(), 60);
					AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
					Test->TestTrue(TEXT("受控零HP进入现有死亡入口"), Candidate->EnterDeath(AbilitySystem->MakeEffectContext()));
					DeathWorldTime = Candidate->GetWorld()->GetTimeSeconds();
					// 复活用游戏时间调度；后台降帧/诊断阻塞不应被误算成已经过了4秒游戏时间。
					RespawnDeadline = DeathWorldTime + Candidate->GetDefinition()->RespawnDelay + 2;
					WallDeadline = FPlatformTime::Seconds() + 45;
					return false;
				}

				if (FPlatformTime::Seconds() > ReadyDeadline)
				{
					Test->AddError(TEXT("有界时间内没有真实已Ready的单玩家Classic PIE；此测试不创建或替代运行比赛。"));
					return true;
				}

				return false;
			}

			if (!Hero->IsAlive())
			{
				if (Hero->GetWorld()->GetTimeSeconds() > RespawnDeadline || FPlatformTime::Seconds() > WallDeadline)
				{
					Test->AddError(FString::Printf(TEXT("真实Hero未复活：GameElapsed=%.3f RespawnDelay=%.3f State=%d"),
						Hero->GetWorld()->GetTimeSeconds() - DeathWorldTime, Hero->GetDefinition()->RespawnDelay, int32(Hero->GetLifeSnapshot().State)));
					return true;
				}

				return false;
			}

			if (!HUD.IsValid() || !HUD->GetPresentedStatus().bCombatReady || HUD->GetPresentedStatus().LifeGeneration != Hero->GetLifeSnapshot().Generation)
			{
				if (Hero->GetWorld()->GetTimeSeconds() < RespawnDeadline && FPlatformTime::Seconds() < WallDeadline)
				{
					return false;
				}

				Test->AddError(TEXT("真实Hero已复活，但同一HUD没有在有界时间内收到本次生命代次。"));
				return true;
			}

			Test->TestTrue(TEXT("复活保留同一个ASC"), Hero->GetHeroAbilitySystem() == AbilitySystem.Get());
			Test->TestEqual(TEXT("复活只推进生命代次一次"), Hero->GetLifeSnapshot().Generation, Generation + 1);
			Test->TestEqual(TEXT("复活保留累计经验"), Hero->GetAttributeComponent()->GetTotalExperiencePoints(), int64(350));
			TestValue(Test, TEXT("复活保留三级"), Hero->GetAttributes()->GetHeroLevel(), 3);
			TestValue(Test, TEXT("复活满生命"), Hero->GetAttributes()->GetHealth(), Hero->GetAttributes()->GetMaxHealth());
			TestValue(Test, TEXT("复活满Mana"), Hero->GetAttributes()->GetMana(), Hero->GetAttributes()->GetMaxMana());
			Test->TestTrue(TEXT("真实PIE复活HUD成长区块仍就绪"), HUD->GetPresentedStatus().bProgressionReady);
			Test->TestEqual(TEXT("真实PIE复活HUD保留三级"), HUD->GetPresentedStatus().HeroLevel, 3);
			Test->TestEqual(TEXT("真实PIE复活HUD保留累计经验350"), HUD->GetPresentedStatus().TotalExperiencePoints, int64(350));
			TestValue(Test, TEXT("真实PIE复活HUD显示本级经验比例"), HUD->GetPresentedStatus().ExperienceProgress, 1.f / 6);
			HUD->ProcessEvent(HUD->FindFunctionChecked(TEXT("ToggleAttributes")), nullptr);
			HUD->ForceLayoutPrepass();
			const auto ViewportWidget = HUD->GetWorld()->GetGameViewport()->GetGameViewportWidget();
			TArray<FColor> Pixels;
			FIntVector ImageSize = FIntVector::ZeroValue;
			if (Test->TestTrue(TEXT("截取含真实UMG的游戏视口"), ViewportWidget.IsValid()
				&& FSlateApplication::Get().TakeScreenshot(ViewportWidget.ToSharedRef(), Pixels, ImageSize)))
			{
				for (FColor& Pixel : Pixels)
				{
					Pixel.A = 255;
				}

				TArray64<uint8> PNG;
				FImageUtils::PNGCompressImageArray(ImageSize.X, ImageSize.Y, Pixels, PNG);
				Test->TestTrue(TEXT("保存本卡真实等级经验与属性HUD截图"), FFileHelper::SaveArrayToFile(PNG,
					*(FPaths::ProjectDir() / TEXT("AI/task-020/classic-attributes-runtime.png"))));
			}

			return true;
		}


	private:
		FAutomationTestBase* Test;
		TWeakObjectPtr<AHeroCharacter> Hero;
		TWeakObjectPtr<UClassicHUDWidget> HUD;
		TWeakObjectPtr<UHeroAbilitySystemComponent> AbilitySystem;
		uint32 Generation = 0;
		double ReadyDeadline;
		double RespawnDeadline = 0;
		double DeathWorldTime = 0;
		double WallDeadline = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributesLiveTest, "Moba.HeroAttributesLive.Lifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributesLiveTest::RunTest(const FString& Parameters)
{
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FHeroAttributeLiveLifecycle(this));
	return true;
}

#endif
