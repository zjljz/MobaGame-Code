#include "Experience/ExperienceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "Items/ItemDefinition.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
	class FBatchExperienceLoading final : public IAutomationLatentCommand
	{
	public:
		explicit FBatchExperienceLoading(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual ~FBatchExperienceLoading() override
		{
			if (World)
			{
				World->GetSubsystem<UExperienceSubsystem>()->StopExperience();
				World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}

		virtual bool Update() override
		{
			if (!World)
			{
				const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
					.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
				World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("BatchExperienceLoading"), nullptr, true, ERHIFeatureLevel::Num, &Values);
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
				Started = FPlatformTime::Seconds();
				auto* System = World->GetSubsystem<UExperienceSubsystem>();
				Test->TestTrue(TEXT("取消前受理批次加载"), System->StartExperience({TEXT("Playlist"), TEXT("Playlist_Batch0811")}) == EExperienceFailure::None);
				CancelledGeneration = System->GetSnapshot().Generation;
				System->StopExperience();
				return false;
			}

			World->Tick(LEVELTICK_All, 0.016f);
			auto* System = World->GetSubsystem<UExperienceSubsystem>();
			const auto Snapshot = System->GetSnapshot();
			if (FPlatformTime::Seconds() - Started > 60 || Snapshot.State == EExperienceState::Failed)
			{
				Test->AddError(FString::Printf(TEXT("批次加载失败或超时 Cycle=%d State=%d Failure=%d"), Cycle, int32(Snapshot.State), int32(Snapshot.Failure)));
				return true;
			}

			if (bWaitingForStop)
			{
				if (Snapshot.State != EExperienceState::Unloaded)
				{
					return false;
				}

				Test->TestFalse(TEXT("取消/停止释放全部资源"), System->HasOwnedResources());
				Test->TestTrue(TEXT("取消淘汰旧加载代次"), Snapshot.Generation > CancelledGeneration);
				if (Cycle > 0)
				{
					FString URL;
					UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL);
					const UExperienceFeatureSubsystem* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
					Test->TestEqual(TEXT("停止后进程租约归零"), Features->GetLeaseCount(URL), 0);
					if (GIsEditor)
					{
						const EGameFeaturePluginState State = UGameFeaturesSubsystem::Get().GetPluginState(URL);
						Test->TestTrue(TEXT("Editor 停止后插件至少 Loaded"), State == EGameFeaturePluginState::Loaded || State == EGameFeaturePluginState::Active);
					}
					else if (Features->GetState(URL) != EExperienceFeatureState::Absent)
					{
						return false;
					}
					else
					{
						Test->TestNull(TEXT("Game 停止后插件已取消注册"), UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL));
					}
				}

				if (Cycle == 3)
				{
					return true;
				}

				const FPrimaryAssetId PlaylistId(TEXT("Playlist"), Cycle == 0 ? TEXT("Playlist_NoItems") : TEXT("Playlist_Batch0811"));
				Test->TestTrue(TEXT("受理当前批次 Playlist"), System->StartExperience(PlaylistId) == EExperienceFailure::None);
				RequestedGeneration = System->GetSnapshot().Generation;
				Test->TestTrue(TEXT("同次请求幂等受理"), System->StartExperience(PlaylistId) == EExperienceFailure::None);
				Test->TestEqual(TEXT("幂等请求不建立新代次"), System->GetSnapshot().Generation, RequestedGeneration);
				bWaitingForStop = false;
				return false;
			}

			if (Snapshot.State != EExperienceState::ExperienceReady)
			{
				return false;
			}

			Test->TestEqual(TEXT("Ready 对应当前加载代次"), Snapshot.Generation, RequestedGeneration);
			const auto* Experience = System->GetExperience();
			if (!Test->TestNotNull(TEXT("Ready 持有 Experience"), Experience))
			{
				return true;
			}

			Test->TestTrue(TEXT("Ready 之前动态地图可见"), System->GetMapInstance() && System->GetMapInstance()->IsLevelLoaded() && System->GetMapInstance()->IsLevelVisible());
			if (Cycle == 0)
			{
				Test->TestFalse(TEXT("NoItems 策略禁用"), Experience->bItemsEnabled);
				Test->TestTrue(TEXT("NoItems 不引用目录"), Experience->ItemCatalog.IsNull());
			}
			else
			{
				Test->TestTrue(TEXT("批次启用物品"), Experience->bItemsEnabled);
				const auto* Catalog = Experience->ItemCatalog.Get();
				if (Test->TestNotNull(TEXT("Ready 已加载目录"), Catalog))
				{
					FString Reason;
					Test->TestTrue(TEXT("Ready 已加载并校验完整 Item/AbilitySet/GA/GE 闭包"), Catalog->ValidateCatalog(Reason));
				}
			}

			++Cycle;
			System->StopExperience();
			bWaitingForStop = true;
			return false;
		}


	private:
		FAutomationTestBase* Test;
		UWorld* World = nullptr;
		double Started = 0;
		int32 Cycle = 0;
		int64 CancelledGeneration = 0;
		int64 RequestedGeneration = 0;
		bool bWaitingForStop = true;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBatchExperienceLoadingTest, "Moba.Experience.Batch0811Loading", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBatchExperienceLoadingTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FBatchExperienceLoading>(this));
	return true;
}

#endif
