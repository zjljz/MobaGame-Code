#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Items/ItemInventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Match/ParticipantState.h"

namespace
{
	class FStatusServerScenario final : public IAutomationLatentCommand
	{
	public:
		explicit FStatusServerScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaStatusEvidence="), Evidence);
		}

		virtual bool Update() override
		{
			if (Evidence.IsEmpty() || FPlatformTime::Seconds() - Started > 150)
			{
				Test->AddError(TEXT("Status Server缺少证据目录或150秒内未完成客户端谓词"));
				return true;
			}

			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->IsGameWorld() && Context.World()->GetNetMode() == NM_DedicatedServer)
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			if (Phase == 0)
			{
				Heroes.Reset();
				Controllers.Reset();
				for (TActorIterator<AHeroCharacter> It(World); It; ++It)
				{
					auto* Controller = Cast<AHeroPlayerController>(It->GetController());
					if (Controller && Controller->GetNetConnection() && It->CanAcceptPlayerInput())
					{
						Heroes.Add(*It);
						Controllers.Add(Controller);
					}
				}

				if (Heroes.Num() != 2)
				{
					return false;
				}

				Test->TestFalse(TEXT("Dedicated Server不装载UI模块"), FModuleManager::Get().IsModuleLoaded(TEXT("MobaGameUI")));
				IFileManager::Get().MakeDirectory(*Evidence, true);
				FString Entities;
				for (int32 Index = 0; Index < Heroes.Num(); ++Index)
				{
					auto* Hero = Heroes[Index].Get();
					FItemRequest Request;
					Request.Entity = Hero->EntityId;
					Request.Epoch = Hero->ConnectionEpoch;
					Request.Sequence = 1;
					Request.Revision = Hero->GetInventory()->GetSnapshot().Revision;
					Request.Operation = EItemOperation::Buy;
					Request.ItemId = TEXT("ArmorPlate");
					const FItemResult Result = Hero->GetInventory()->Execute(Controllers[Index].Get(), Request);
					if (!Test->TestTrue(TEXT("测试服务器通过既有交易入口给两拥有端各买一件"), Result.bAccepted))
					{
						return true;
					}

					Entities += Hero->EntityId.ToString(EGuidFormats::Digits) + TEXT("\n");
				}
				FFileHelper::SaveStringToFile(Entities, *(Evidence / TEXT("seeded.txt")));
				Phase = 1;
			}

			if (Heroes.ContainsByPredicate([](const auto& Hero) { return !Hero.IsValid(); })
				|| Controllers.ContainsByPredicate([](const auto& Controller) { return !Controller.IsValid(); }))
			{
				Test->AddError(TEXT("Status网络夹具实体提前失效"));
				return true;
			}

			if (Phase == 1 && AllAcknowledged(TEXT("seeded")))
			{
				for (const auto& Hero : Heroes)
				{
					auto* Abilities = Hero->GetHeroAbilitySystem();
					Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
					Test->TestTrue(TEXT("真实服务器死亡"), Hero->EnterDeath(Abilities->MakeEffectContext()));
				}
				Phase = 2;
			}

			if (Phase == 2 && AllAcknowledged(TEXT("dead")) && AllAcknowledged(TEXT("respawn")))
			{
				for (const auto& Controller : Controllers)
				{
					Controller->UnPossess();
				}
				WriteMarker(TEXT("unbound"));
				Phase = 3;
			}

			if (Phase == 3 && AllAcknowledged(TEXT("unbound")))
			{
				for (int32 Index = 0; Index < Heroes.Num(); ++Index)
				{
					auto* Hero = Heroes[Index].Get();
					auto* Controller = Controllers[Index].Get();
					auto* Participant = Controller->GetPlayerState<AParticipantState>();
					++Participant->ConnectionEpoch;
					Hero->ConnectionEpoch = Participant->ConnectionEpoch;
					Controller->Possess(Hero);
					Controller->ClientBindingState(Hero->EntityId, Hero->ConnectionEpoch, Hero->GetLifeSnapshot().Generation, Hero->GetLifeSnapshot().DeathCount);
					Participant->ForceNetUpdate();
					Hero->ForceNetUpdate();
				}
				Phase = 4;
			}

			if (Phase == 4 && AllAcknowledged(TEXT("rebound")))
			{
				WriteMarker(TEXT("finished"));
				return true;
			}

			return false;
		}

	private:
		bool AllAcknowledged(const TCHAR* Stage) const
		{
			for (const auto& Hero : Heroes)
			{
				const FString File = FString(Stage) + TEXT("-") + Hero->EntityId.ToString(EGuidFormats::Digits) + TEXT(".txt");
				if (!IFileManager::Get().FileExists(*(Evidence / File)))
				{
					return false;
				}
			}
			return true;
		}

		void WriteMarker(const TCHAR* Name) const
		{
			FFileHelper::SaveStringToFile(TEXT("observed"), *(Evidence / (FString(Name) + TEXT(".txt"))));
		}

		FAutomationTestBase* Test;
		double Started;
		FString Evidence;
		int32 Phase = 0;
		TArray<TWeakObjectPtr<AHeroCharacter>> Heroes;
		TArray<TWeakObjectPtr<AHeroPlayerController>> Controllers;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusServerTest, "Moba.UI.StatusServer", EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FStatusServerTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FStatusServerScenario(this));
	return true;
}

#endif
