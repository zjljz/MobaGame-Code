#include "Experience/ExperienceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "GameFramework/GameModeBase.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	class FPIEContentResidencyScenario final : public IAutomationLatentCommand
	{
	public:
		explicit FPIEContentResidencyScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}

		virtual ~FPIEContentResidencyScenario() override
		{
			FEditorDelegates::ShutdownPIE.Remove(ShutdownHandle);
			if (bOwnsSession)
			{
				GEditor->RequestEndPlayMap();
			}

			if (GuardLease.IsValid())
			{
				Features->Release(URL, GuardLease);
			}

			if (OpenedAsset.IsValid())
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(OpenedAsset.Get());
			}
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 180.0)
			{
				Test->AddError(FString::Printf(TEXT("真实 PIE 驻留测试超时 Round=%d Stage=%d；失败场景须隔离收尾"), Round, Stage));
				return true;
			}

			switch (Stage)
			{
			case 0:
				Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
				if (!GIsEditor || GEditor->PlayWorld || !UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL)
					|| Features->GetLeaseCount(URL) != 0)
				{
					Test->AddError(TEXT("只在隔离 Editor 自动化进程运行；不能接管已有 PIE 或项目租约"));
					return true;
				}

				ShutdownHandle = FEditorDelegates::ShutdownPIE.AddLambda([this](bool bIsSimulating)
					{
						bObservedShutdown = true;
						Test->TestEqual(TEXT("真实 PIE 完成后只保留其他使用者的租约"), Features->GetLeaseCount(URL), Round == 0 ? 1 : 0);
						Test->TestTrue(TEXT("PIE 完成没有降级插件"), IsNativeResident());
					});
				GuardLease = FGuid::NewGuid();
				if (!Test->TestTrue(TEXT("另一个使用者的内容租约受理"), Features->Acquire(TEXT("MobaClassic"), GuardLease, URL)))
				{
					return true;
				}

				++Stage;
				return false;
			case 1:
				if (Features->GetState(URL) == EExperienceFeatureState::Failed)
				{
					Test->AddError(TEXT("PIE 测试内容插件加载失败"));
					return true;
				}

				if (Features->GetState(URL) != EExperienceFeatureState::Loaded)
				{
					return false;
				}

				Settings.Reset(NewObject<ULevelEditorPlaySettings>());
				Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
				Settings->SetPlayNumberOfClients(1);
				{
					FRequestPlaySessionParams Params;
					Params.EditorPlaySettings = Settings.Get();
					Params.GlobalMapOverride = TEXT("/Engine/Maps/Entry");
					Params.GameModeOverride = AGameModeBase::StaticClass();
					Params.bAllowOnlineSubsystem = false;
					GEditor->RequestPlaySession(Params);
				}

				bOwnsSession = true;
				bObservedShutdown = false;
				++Stage;
				return false;
			case 2:
				if (!GEditor->PlayWorld)
				{
					return false;
				}

				PIEWorld = GEditor->PlayWorld;
				{
					UExperienceSubsystem* System = PIEWorld->GetSubsystem<UExperienceSubsystem>();
					Test->TestTrue(TEXT("真正 PIE World"), PIEWorld->WorldType == EWorldType::PIE);
					if (!Test->TestTrue(TEXT("PIE 内容受理"), System->StartExperience({TEXT("Playlist"), TEXT("Playlist_FrameworkTest")}) == EExperienceFailure::None))
					{
						return true;
					}

					if (Round == 2)
					{
						Test->TestTrue(TEXT("实际内容加载未完成时退出"), System->GetSnapshot().State == EExperienceState::Loading
							|| System->GetSnapshot().State == EExperienceState::Activating);
						GEditor->RequestEndPlayMap();
						Stage = 4;
						return false;
					}
				}

				++Stage;
				return false;
			case 3:
				{
					UExperienceSubsystem* System = PIEWorld.IsValid() ? PIEWorld->GetSubsystem<UExperienceSubsystem>() : nullptr;
					if (!System || System->GetSnapshot().State == EExperienceState::Failed)
					{
						Test->AddError(TEXT("PIE 内容加载失败或 World 提前结束"));
						return true;
					}

					if (System->GetSnapshot().State != EExperienceState::ExperienceReady)
					{
						return false;
					}

					Test->TestTrue(TEXT("真正动态关卡 LoadedVisible 后退出"), System->GetMapInstance() && System->GetMapInstance()->GetLoadedLevel() && System->GetMapInstance()->IsLevelVisible());
					Test->TestEqual(TEXT("本 PIE 独立持有内容租约"), Features->GetLeaseCount(URL), Round == 0 ? 2 : 1);
					if (Round == 0)
					{
						UObject* Asset = const_cast<UExperienceDefinition*>(System->GetExperience());
						UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
						if (!Test->TestNull(TEXT("不接管已有资产编辑器"), Editors->FindEditorForAsset(Asset, false)))
						{
							return true;
						}

						if (!Test->TestTrue(TEXT("真实打开资产编辑器，退出 PIE 时保持打开"), Editors->OpenEditorForAsset(Asset)))
						{
							return true;
						}

						OpenedAsset = Asset;
					}

					// 不先 Stop，不 flush 或强制 GC；走用户点击停止 PIE 的真实退出流程。
					GEditor->RequestEndPlayMap();
				}

				++Stage;
				return false;
			case 4:
				if (!bObservedShutdown || GEditor->PlayWorld)
				{
					return false;
				}

				bOwnsSession = false;
				Test->TestFalse(TEXT("原 PIE World 没有被内容驻留反向保活"), PIEWorld.IsValid());
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					Test->TestFalse(TEXT("退出后没有本次 PIE 上下文"), Context.World() && Context.World()->WorldType == EWorldType::PIE);
				}

				Test->TestTrue(TEXT("资产编辑器仍能持有同一定义资产"), OpenedAsset.IsValid()
					&& GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(OpenedAsset.Get(), false));
				if (GuardLease.IsValid())
				{
					Test->TestEqual(TEXT("另一个使用者没有受 PIE 退出影响"), Features->GetLeaseCount(URL), 1);
					Features->Release(URL, GuardLease);
					GuardLease.Invalidate();
				}

				Test->TestEqual(TEXT("全部项目租约归零"), Features->GetLeaseCount(URL), 0);
				Test->TestTrue(TEXT("零租约插件明确驻留，供后续 PIE 复用"), IsNativeResident());
				if (++Round < 3)
				{
					Stage = 1;
					return false;
				}

				return true;
			}

			return false;
		}


	private:
		bool IsNativeResident() const
		{
			const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
			return NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active;
		}


	private:
		FAutomationTestBase* Test;
		UExperienceFeatureSubsystem* Features = nullptr;
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		TWeakObjectPtr<UWorld> PIEWorld;
		TWeakObjectPtr<UObject> OpenedAsset;
		FDelegateHandle ShutdownHandle;
		FString URL;
		FGuid GuardLease;
		double Started;
		int32 Stage = 0;
		int32 Round = 0;
		bool bOwnsSession = false;
		bool bObservedShutdown = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPIEContentResidencyTest, "Moba.Experience.Residency.PIELoadedExitRepeatAndCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FPIEContentResidencyTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPIEContentResidencyScenario>(this));
	return true;
}

#endif
