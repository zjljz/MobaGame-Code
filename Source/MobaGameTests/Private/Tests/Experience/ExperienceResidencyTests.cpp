#include "Experience/ExperienceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/ExperienceDefinitions.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "GameFeaturePluginOperationResult.h"
#include "Misc/AutomationTest.h"
#include "MobaMessageTags.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#endif

// 只读观察真实请求的句柄和接入回调；不伪造加载完成、原生状态或 World teardown。
struct FExperienceResidencyTestAccess
{
	static void CheckClean(FAutomationTestBase& Test, const UExperienceSubsystem& System)
	{
		Test.TestFalse(TEXT("全部 World 自有资源已释放"), System.HasOwnedResources());
		Test.TestEqual(TEXT("没有资产句柄"), System.Handles.Num(), 0);
		Test.TestFalse(TEXT("没有 Feature 租约"), System.FeatureLease.IsValid());
		Test.TestTrue(TEXT("没有 Feature URL"), System.FeatureURL.IsEmpty());
		Test.TestNull(TEXT("没有 Playlist 强引用"), System.Playlist.Get());
		Test.TestNull(TEXT("没有 Experience 强引用"), System.Experience.Get());
		Test.TestNull(TEXT("没有 MapDefinition 强引用"), System.Map.Get());
		Test.TestNull(TEXT("没有地图强引用"), System.MapInstance.Get());
		Test.TestFalse(TEXT("没有 AssetRegistry 回调"), System.RegistryReadyHandle.IsValid());
		Test.TestFalse(TEXT("没有 streaming 回调"), System.StreamingStateHandle.IsValid());
		Test.TestFalse(TEXT("没有加载超时回调"), System.LoadTimeout.IsValid());
		Test.TestFalse(TEXT("没有延后地图回调"), System.MapEventTimer.IsValid());
	}
};

namespace
{
	bool IsNativeResident(const FString& URL)
	{
		const EGameFeaturePluginState State = UGameFeaturesSubsystem::Get().GetPluginState(URL);
		return State == EGameFeaturePluginState::Loaded || State == EGameFeaturePluginState::Active;
	}

	class FExperienceResidencyScenario final : public IAutomationLatentCommand
	{
	public:
		FExperienceResidencyScenario(FAutomationTestBase* InTest, bool bInNormalGameStop, bool bInAssetEditor)
			: Test(InTest), bNormalGameStop(bInNormalGameStop), bAssetEditor(bInAssetEditor) {}

		virtual ~FExperienceResidencyScenario() override
		{
#if WITH_EDITOR
			if (OpenedAsset.IsValid() && GEditor)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(OpenedAsset.Get());
			}
#endif
			Destroy(WorldA);
			Destroy(WorldB);
		}

		virtual bool Update() override
		{
			if (Stage == 0)
			{
				Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
				if (!Test->TestTrue(TEXT("原生 Classic URL"), UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL))
					|| !Test->TestEqual(TEXT("隔离进程无其他使用者"), Features->GetLeaseCount(URL), 0))
				{
					return true;
				}

				if (bNormalGameStop && (!Test->TestFalse(TEXT("必须在真正非 Editor Game/Server 进程执行"), GIsEditor)
					|| !Test->TestTrue(TEXT("正常 Game 卸载测试须从未驻留的进程开始"), Features->GetState(URL) == EExperienceFeatureState::Absent && !IsNativeResident(URL))))
				{
					return true;
				}

				Started = FPlatformTime::Seconds();
				WorldA = CreateWorld(TEXT("ResidencyA"));
				WorldB = CreateWorld(TEXT("ResidencyB"));
				Start(WorldA);
				Start(WorldB);
				Stage = 1;
				return false;
			}

			if (WorldA)
			{
				WorldA->Tick(LEVELTICK_All, 0.016f);
			}

			if (WorldB)
			{
				WorldB->Tick(LEVELTICK_All, 0.016f);
			}

			if (Test->HasAnyErrors() || FPlatformTime::Seconds() - Started > 90)
			{
				Test->AddError(FString::Printf(TEXT("驻留场景失败/超时 Stage=%d"), Stage));
				return true;
			}

			UExperienceSubsystem* A = WorldA ? WorldA->GetSubsystem<UExperienceSubsystem>() : nullptr;
			UExperienceSubsystem* B = WorldB ? WorldB->GetSubsystem<UExperienceSubsystem>() : nullptr;
			if ((A && A->GetSnapshot().State == EExperienceState::Failed) || (B && B->GetSnapshot().State == EExperienceState::Failed))
			{
				Test->AddError(TEXT("真实 Experience 加载失败"));
				return true;
			}

			switch (Stage)
			{
			case 1:
				if (!Ready(A) || !Ready(B))
				{
					return false;
				}

				Test->TestEqual(TEXT("两个 World 的独立租约"), Features->GetLeaseCount(URL), 2);
				Test->TestTrue(TEXT("地图实例独立"), A->GetMapInstance() != B->GetMapInstance());
#if WITH_EDITOR
				if (bAssetEditor)
				{
					UObject* Asset = const_cast<UExperienceDefinition*>(A->GetExperience());
					UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
					if (!Test->TestNull(TEXT("隔离 fixture 不接管已有资产编辑器"), Editors->FindEditorForAsset(Asset, false)))
					{
						return true;
					}

					OpenedAsset = Asset;
					if (!Test->TestTrue(TEXT("真实打开资产编辑器"), Editors->OpenEditorForAsset(Asset)))
					{
						return true;
					}
				}
#endif
				if (bNormalGameStop)
				{
					A->StopExperience();
					Test->TestTrue(TEXT("实际地图移除前仍 Stopping"), A->GetSnapshot().State == EExperienceState::Stopping);
					Test->TestEqual(TEXT("移除屏障前保留两个租约"), Features->GetLeaseCount(URL), 2);
				}
				else
				{
					Destroy(WorldA);
				}

				Stage = 2;
				return false;
			case 2:
				if (A && A->GetSnapshot().State != EExperienceState::Unloaded)
				{
					return false;
				}

				if (A)
				{
					FExperienceResidencyTestAccess::CheckClean(*Test, *A);
					Test->TestEqual(TEXT("正常 Stop 地图已移出 World"), WorldA->GetStreamingLevels().Num(), 0);
				}

				Test->TestTrue(TEXT("另一个 World 不受影响"), Ready(B) && B->GetMapInstance()->IsLevelVisible());
				Test->TestEqual(TEXT("只归还 A 的租约"), Features->GetLeaseCount(URL), 1);
				Test->TestTrue(TEXT("其他 World 使用时原生插件驻留"), IsNativeResident(URL));
				B->StopExperience();
				Stage = 3;
				return false;
			case 3:
				if (B->GetSnapshot().State != EExperienceState::Unloaded || (bNormalGameStop && Features->GetState(URL) != EExperienceFeatureState::Absent))
				{
					return false;
				}

				FExperienceResidencyTestAccess::CheckClean(*Test, *B);
				Test->TestEqual(TEXT("全部租约归零"), Features->GetLeaseCount(URL), 0);
				Test->TestEqual(TEXT("B 地图已移出 World"), WorldB->GetStreamingLevels().Num(), 0);
				if (bNormalGameStop)
				{
					Test->TestNull(TEXT("正常 Game 最后租约完整取消注册"), UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL));
					Test->TestFalse(TEXT("正常 Game 无 GameFeatureData 原生句柄"), UAssetManager::Get().GetPrimaryAssetHandle({TEXT("GameFeatureData"), TEXT("MobaClassic")}).IsValid());
				}
				else
				{
					Test->TestTrue(TEXT("Editor/强制 teardown 后原生状态至少 Loaded"), IsNativeResident(URL));
#if WITH_EDITOR
					if (bAssetEditor)
					{
						Test->TestTrue(TEXT("资产编辑器仍持有同一资产"), OpenedAsset.IsValid() && GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(OpenedAsset.Get(), false));
					}
#endif
				}

				Start(WorldB);
				Stage = 4;
				return false;
			case 4:
				if (!Ready(B))
				{
					return false;
				}

				Test->TestEqual(TEXT("同进程再启动只有一个租约"), Features->GetLeaseCount(URL), 1);
				Test->TestEqual(TEXT("B 两轮各一次 Ready，无旧回调复活"), ReadyB, 2);
				B->StopExperience();
				Stage = 5;
				return false;
			case 5:
				if (B->GetSnapshot().State != EExperienceState::Unloaded || (bNormalGameStop && Features->GetState(URL) != EExperienceFeatureState::Absent))
				{
					return false;
				}

				FExperienceResidencyTestAccess::CheckClean(*Test, *B);
				Test->TestEqual(TEXT("第二轮结束租约归零"), Features->GetLeaseCount(URL), 0);
				Test->TestTrue(TEXT("第二轮遵循同一卸载/驻留合同"), bNormalGameStop
					? UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr : IsNativeResident(URL));
				return true;
			}

			return false;
		}

	private:
		UWorld* CreateWorld(FName Name)
		{
			const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			GMP::FMessageUtils::ListenObjectMessage(World->GetSubsystem<UExperienceSubsystem>(), MOBA_MESSAGE_EXPERIENCE_CHANGED, World,
				[this, World](const FExperienceSnapshot& Snapshot)
				{
					if (Snapshot.State == EExperienceState::ExperienceReady && World == WorldB)
					{
						++ReadyB;
					}
				});
			return World;
		}

		void Start(UWorld* World)
		{
			Test->TestTrue(TEXT("真实 Playlist 加载受理"), World->GetSubsystem<UExperienceSubsystem>()->StartExperience({TEXT("Playlist"), TEXT("Playlist_FrameworkTest")}) == EExperienceFailure::None);
		}

		void Destroy(UWorld*& World)
		{
			if (!World)
			{
				return;
			}

			UExperienceSubsystem* System = World->GetSubsystem<UExperienceSubsystem>();
			GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, World);
			bool bObservedCleanup = false;
			const FDelegateHandle CleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda(
				[this, World, System, &bObservedCleanup](UWorld* CleaningWorld, bool, bool)
				{
					if (CleaningWorld == World)
					{
						bObservedCleanup = true;
						Test->TestTrue(TEXT("真实 cleanup 回调中的重入 Start 被拒绝"),
							System->StartExperience({TEXT("Playlist"), TEXT("Playlist_FrameworkTest")}) == EExperienceFailure::WorldUnavailable);
					}
				});
			World->DestroyWorld(false);
			FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
			Test->TestTrue(TEXT("实际经过原生 World cleanup"), bObservedCleanup);
			FExperienceResidencyTestAccess::CheckClean(*Test, *System);
			GEngine->DestroyWorldContext(World);
			World = nullptr;
		}

		static bool Ready(const UExperienceSubsystem* System)
		{
			return System && System->GetSnapshot().State == EExperienceState::ExperienceReady;
		}

	private:
		FAutomationTestBase* Test;
		UExperienceFeatureSubsystem* Features = nullptr;
		UWorld* WorldA = nullptr;
		UWorld* WorldB = nullptr;
		TWeakObjectPtr<UObject> OpenedAsset;
		FString URL;
		double Started = 0;
		int32 Stage = 0;
		int32 ReadyB = 0;
		bool bNormalGameStop;
		bool bAssetEditor;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceGameStopTest, "Moba.Experience.Residency.GameNormalStop", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FExperienceGameStopTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceResidencyScenario>(this, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceGameTeardownTest, "Moba.Experience.Residency.GameForcedTeardown", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FExperienceGameTeardownTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceResidencyScenario>(this, false, false));
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceEditorResidencyTest, "Moba.Experience.Residency.EditorWorlds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExperienceEditorResidencyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("真实 Editor 进程"), GIsEditor);
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceResidencyScenario>(this, false, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceAssetEditorResidencyTest, "Moba.Experience.Residency.AssetEditorOpen", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FExperienceAssetEditorResidencyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("真实 Editor 进程"), GIsEditor);
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceResidencyScenario>(this, false, true));
	return true;
}
#endif

namespace
{
	enum class ENativeResidencyCase : uint8
	{
		CancelLoading,
		ExternalLoaded,
		ExternalActive
	};

	struct FNativeResidencyCompletion
	{
		bool bComplete = false;
		bool bError = false;
	};

	class FNativeResidencyScenario final : public IAutomationLatentCommand
	{
	public:
		FNativeResidencyScenario(FAutomationTestBase* InTest, ENativeResidencyCase InCase) : Test(InTest), Case(InCase) {}

		virtual ~FNativeResidencyScenario() override
		{
			if (Lease.IsValid())
			{
				Features->Release(URL, Lease, EExperienceFeatureReleasePolicy::KeepLoaded);
			}
		}

		virtual bool Update() override
		{
			if (!bStarted)
			{
				Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
				if (!Test->TestTrue(TEXT("Classic URL 可解析"), UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL))
					|| !Test->TestEqual(TEXT("隔离原生场景没有租约"), Features->GetLeaseCount(URL), 0)
					|| !Test->TestTrue(TEXT("原生场景需要 fresh 进程且尚无本系统条目"), Features->GetState(URL) == EExperienceFeatureState::Absent))
				{
					return true;
				}

				bStarted = true;
				Started = FPlatformTime::Seconds();
				if (Case == ENativeResidencyCase::CancelLoading)
				{
					Lease = FGuid::NewGuid();
					Test->TestTrue(TEXT("真实 Feature 加载受理"), Features->Acquire(TEXT("MobaClassic"), Lease, URL));
					// 若当前平台同步完成，明确失败并补本地冷启动覆盖，不冒充加载中取消。
					Test->TestTrue(TEXT("已实际观察到 Loading 窗口"), Features->GetState(URL) == EExperienceFeatureState::Loading);
					Features->Release(URL, Lease, EExperienceFeatureReleasePolicy::KeepLoaded);
					Lease.Invalidate();
					Test->TestEqual(TEXT("完成回调前租约已归零"), Features->GetLeaseCount(URL), 0);
				}
				else
				{
					const TSharedRef<FNativeResidencyCompletion> Result = Completion;
					const auto OnComplete = FGameFeaturePluginLoadComplete::CreateLambda([Result](const UE::GameFeatures::FResult& OperationResult)
						{
							Result->bError = OperationResult.HasError();
							Result->bComplete = true;
						});
					if (Case == ENativeResidencyCase::ExternalActive)
					{
						UGameFeaturesSubsystem::Get().LoadAndActivateGameFeaturePlugin(URL, OnComplete);
					}
					else
					{
						UGameFeaturesSubsystem::Get().LoadGameFeaturePlugin(URL, OnComplete);
					}
				}

				return false;
			}

			if (Test->HasAnyErrors() || Completion->bError || FPlatformTime::Seconds() - Started > 60)
			{
				Test->AddError(TEXT("真实原生驻留场景失败或超时；未跳过此覆盖"));
				return true;
			}

			if ((Case == ENativeResidencyCase::CancelLoading && Features->GetState(URL) != EExperienceFeatureState::Loaded)
				|| (Case != ENativeResidencyCase::CancelLoading && !Completion->bComplete))
			{
				return false;
			}

			const EGameFeaturePluginState Expected = Case == ENativeResidencyCase::ExternalActive ? EGameFeaturePluginState::Active : EGameFeaturePluginState::Loaded;
			Test->TestTrue(TEXT("原生状态保持要求的驻留下限"), UGameFeaturesSubsystem::Get().GetPluginState(URL) == Expected);
			Test->TestEqual(TEXT("取消/借用之前没有遗留租约"), Features->GetLeaseCount(URL), 0);
			if (!bReacquired)
			{
				Lease = FGuid::NewGuid();
				Test->TestTrue(TEXT("驻留插件可重新 acquire"), Features->Acquire(TEXT("MobaClassic"), Lease, URL));
				Test->TestTrue(TEXT("借用内容 Ready"), Features->GetState(URL) == EExperienceFeatureState::Loaded);
				Test->TestEqual(TEXT("只有新的单个租约"), Features->GetLeaseCount(URL), 1);
				Features->Release(URL, Lease);
				Lease.Invalidate();
				bReacquired = true;
				return false;
			}

			Test->TestEqual(TEXT("重新释放租约归零"), Features->GetLeaseCount(URL), 0);
			Test->TestTrue(TEXT("后续默认 Release 未撤销驻留/外部所有权"), UGameFeaturesSubsystem::Get().GetPluginState(URL) == Expected);
			Test->TestTrue(TEXT("仍保留原生 GameFeatureData 句柄"), UAssetManager::Get().GetPrimaryAssetHandle({TEXT("GameFeatureData"), TEXT("MobaClassic")}).IsValid());
			return true;
		}

	private:
		FAutomationTestBase* Test;
		ENativeResidencyCase Case;
		UExperienceFeatureSubsystem* Features = nullptr;
		TSharedRef<FNativeResidencyCompletion> Completion = MakeShared<FNativeResidencyCompletion>();
		FString URL;
		FGuid Lease;
		double Started = 0;
		bool bStarted = false;
		bool bReacquired = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureResidentCancellationTest, "Moba.Experience.Residency.CancelFeatureLoading", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFeatureResidentCancellationTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNativeResidencyScenario>(this, ENativeResidencyCase::CancelLoading));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureBorrowLoadedTest, "Moba.Experience.Residency.BorrowExternalLoaded", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FFeatureBorrowLoadedTest::RunTest(const FString& Parameters)
{
	if (!TestFalse(TEXT("所有权测试必须非 Editor，不能让 Editor 驻留策略掩盖越权卸载"), GIsEditor))
	{
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNativeResidencyScenario>(this, ENativeResidencyCase::ExternalLoaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureBorrowActiveTest, "Moba.Experience.Residency.BorrowExternalActive", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FFeatureBorrowActiveTest::RunTest(const FString& Parameters)
{
	if (!TestFalse(TEXT("所有权测试必须非 Editor，不能让 Editor 驻留策略掩盖越权卸载"), GIsEditor))
	{
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNativeResidencyScenario>(this, ENativeResidencyCase::ExternalActive));
	return true;
}

#endif
