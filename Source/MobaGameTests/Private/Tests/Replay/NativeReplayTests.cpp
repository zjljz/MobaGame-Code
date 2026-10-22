#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Replay/ReplayStateProbe.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ReplaySubsystem.h"
#include "String/LexFromString.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* ReplayName = TEXT("NativeActor");
	constexpr int32 SampleCount = 3;
	constexpr double TimeoutSeconds = 60.0;

	FVector SamplePosition(int32 Index)
	{
		return FVector(100.0 + Index * 300.0, -200.0 + Index * 150.0, 300.0 + Index * 50.0);
	}

	class FNativeReplayScenario final : public IAutomationLatentCommand
	{
	public:
		FNativeReplayScenario(FAutomationTestBase* InTest, bool bInPlayback, FString InDirectory, float InSampleDuration)
			: Test(InTest), bPlayback(bInPlayback), Directory(MoveTemp(InDirectory)), Started(FPlatformTime::Seconds()), SampleDuration(InSampleDuration)
		{
			LastAdvanceWallSeconds = Started;
		}

		virtual ~FNativeReplayScenario() override
		{
			Cleanup();
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > TimeoutSeconds)
			{
				return Fail(TEXT("Timeout"));
			}

			if (!bInitialized)
			{
				return Initialize();
			}

			UGameInstance* Instance = GameInstance.Get();
			UWorld* World = Instance ? Instance->GetWorld() : nullptr;
			UReplaySubsystem* Replay = Instance ? Instance->GetSubsystem<UReplaySubsystem>() : nullptr;
			if (!Instance || !Replay)
			{
				return Fail(TEXT("InstanceOrSubsystemLost"));
			}

			if (!World)
			{
				return bActiveObserved ? Fail(TEXT("ActiveWorldLost")) : false;
			}

			LastReplayTime = Replay->GetReplayCurrentTime();
			if (bStopping)
			{
				if (Replay->IsRecording() || Replay->IsPlaying() || World->GetDemoNetDriver())
				{
					return false;
				}

				if (!bPlayback)
				{
					// 该标记只证明原生停止谓词成立；文件写入可用性由下一独立进程真正读回验证。
					const FString Recorder = FString::Printf(TEXT("%u"), FPlatformProcess::GetCurrentProcessId());
					if (!FFileHelper::SaveStringToFile(Recorder, *FPaths::Combine(Directory, TEXT("Record.complete")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						return Fail(TEXT("RecordReceiptWriteFailed"));
					}
				}

				Test->AddInfo(FString::Printf(TEXT("NativeReplay %s完成：Samples=%d HoldSeconds=%.0f Duration=%.3f PID=%u"), bPlayback ? TEXT("Playback") : TEXT("RecordStop"),
					SampleCount, SampleDuration, CompletedReplayTime, FPlatformProcess::GetCurrentProcessId()));
				Cleanup();
				return true;
			}

			UDemoNetDriver* Driver = World->GetDemoNetDriver();
			if (!Driver || (bPlayback ? !Replay->IsPlaying() : !Replay->IsRecording()))
			{
				// PlayReplay 接受后，原生 PendingNetGame/异步地图加载可能暂时移走 World 上的 driver。
				return bActiveObserved ? Fail(TEXT("ActiveReplayLost")) : false;
			}

			if (!bActiveObserved && Replay->GetActiveReplayName().IsEmpty())
			{
				return false;
			}

			if (Replay->GetActiveReplayName() != ReplayName || (bPlayback ? !Driver->IsPlaying() : !Driver->IsRecording()))
			{
				return Fail(TEXT("ReplayIdentityOrDriverModeMismatch"));
			}

			if (LastReplayTime < PreviousReplayTime)
			{
				return Fail(TEXT("ReplayTimeRegressed"));
			}

			if (LastReplayTime > PreviousReplayTime)
			{
				LastAdvanceWallSeconds = FPlatformTime::Seconds();
			}

			if (bPlayback && FPlatformTime::Seconds() - LastAdvanceWallSeconds > 5.0)
			{
				return Fail(TEXT("PlaybackClockStalled"));
			}

			PreviousReplayTime = LastReplayTime;
			if (!World->HasBegunPlay() || LastReplayTime <= 0)
			{
				return false;
			}

			bActiveObserved = true;
			return bPlayback ? ObservePlayback(World) : AdvanceRecording(World);
		}


	private:
		bool Initialize()
		{
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay())
				{
					if (World)
					{
						return Fail(TEXT("AmbiguousGameWorld"));
					}

					World = Context.World();
				}
			}

			if (!World)
			{
				return false;
			}

			// 空引擎地图与原生入口隔离项目准入/凭证路径；不用真实比赛冒充最小 fixture。
			AGameModeBase* Mode = World->GetAuthGameMode();
			UGameInstance* Instance = World->GetGameInstance();
			if (World->GetNetMode() != NM_Standalone || World->GetOutermost()->GetName() != TEXT("/Engine/Maps/Entry")
				|| !Mode || Mode->GetClass() != AGameModeBase::StaticClass() || !Instance || Instance->GetClass() != UGameInstance::StaticClass())
			{
				return Fail(TEXT("RequiresEntryStandaloneNativeGameModeAndGameInstance"));
			}

			for (TActorIterator<APlayerController> It(World); It; ++It)
			{
				if (It->GetClass() != APlayerController::StaticClass())
				{
					return Fail(TEXT("NonNativeControllerPresent"));
				}
			}

			for (TActorIterator<AReplayStateProbe> It(World); It; ++It)
			{
				return Fail(TEXT("ProbeAlreadyPresent"));
			}

			UReplaySubsystem* Replay = Instance->GetSubsystem<UReplaySubsystem>();
			if (!Replay || Replay->IsRecording() || Replay->IsPlaying() || World->GetDemoNetDriver())
			{
				return Fail(TEXT("ReplaySubsystemUnavailableOrBusy"));
			}

			const FString ReceiptPath = FPaths::Combine(Directory, TEXT("Record.complete"));
			const FString DemoPath = FPaths::Combine(Directory, TEXT("NativeActor.replay"));
			if (bPlayback)
			{
				FString Recorder;
				uint32 RecorderProcess = 0;
				if (!FFileHelper::LoadFileToString(Recorder, *ReceiptPath) || !LexTryParseString(RecorderProcess, *Recorder) || RecorderProcess == 0
					|| RecorderProcess == FPlatformProcess::GetCurrentProcessId() || IFileManager::Get().FileSize(*DemoPath) <= 0)
				{
					return Fail(TEXT("RequiresCompletedRecordingFromDifferentProcess"));
				}
			}
			else if (IFileManager::Get().FileExists(*ReceiptPath) || IFileManager::Get().FileExists(*DemoPath))
			{
				return Fail(TEXT("RunAlreadyExistsChooseNewGuid"));
			}

			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				return Fail(TEXT("OutputDirectoryUnavailable"));
			}

			GameInstance = Instance;
			bOriginalLoadDefaultMapOnStop = Replay->bLoadDefaultMapOnStop;
			Replay->bLoadDefaultMapOnStop = false;
			bInitialized = true;
			bOwnReplay = true;
			const TArray<FString> Options = {TEXT("ReplayStreamerOverride=LocalFileNetworkReplayStreaming"), TEXT("ReplayStreamerDemoPath=") + Directory + TEXT("/")};
			if (bPlayback)
			{
				if (!Instance->PlayReplay(ReplayName, nullptr, Options))
				{
					return Fail(TEXT("PlayReplayRejected"));
				}
			}
			else
			{
				Instance->StartRecordingReplay(ReplayName, TEXT("TASK-015 native actor fixture"), Options);
			}

			return false;
		}

		bool AdvanceRecording(UWorld* World)
		{
			if (!Probe.IsValid())
			{
				if (Sample != INDEX_NONE)
				{
					return Fail(TEXT("RecordingProbeLost"));
				}

				Probe = World->SpawnActor<AReplayStateProbe>();
				if (!Probe.IsValid() || !Probe->HasAuthority())
				{
					return Fail(TEXT("RecordingProbeSpawnFailed"));
				}
			}

			if (Sample != INDEX_NONE && LastReplayTime - SampleStarted < SampleDuration)
			{
				return false;
			}

			if (Sample == SampleCount - 1)
			{
				Stop();
				return false;
			}

			++Sample;
			Probe->SampleIndex = Sample;
			if (!Probe->SetActorLocation(SamplePosition(Sample), false, nullptr, ETeleportType::TeleportPhysics))
			{
				return Fail(TEXT("RecordingProbeMoveFailed"));
			}

			Probe->ForceNetUpdate();
			SampleStarted = LastReplayTime;
			Test->AddInfo(FString::Printf(TEXT("RecordSample Index=%d Time=%.3f Position=%s"), Sample, LastReplayTime, *Probe->GetActorLocation().ToString()));
			return false;
		}

		bool ObservePlayback(UWorld* World)
		{
			AReplayStateProbe* Observed = nullptr;
			for (TActorIterator<AReplayStateProbe> It(World); It; ++It)
			{
				if (Observed)
				{
					return Fail(TEXT("DuplicatePlaybackProbe"));
				}

				Observed = *It;
			}

			if (!Observed)
			{
				return false;
			}

			if (Observed->HasAuthority() || (Sample != INDEX_NONE && Probe.Get() != Observed))
			{
				return Fail(TEXT("PlaybackMustRetainSameNonAuthorityActor"));
			}

			if (Sample == SampleCount - 1)
			{
				if (LastReplayTime - SampleStarted >= 0.25f)
				{
					Stop();
				}

				return false;
			}

			const int32 Expected = Sample + 1;
			if (Observed->SampleIndex > Expected)
			{
				return Fail(TEXT("PlaybackSkippedRequiredSample"));
			}

			if (LastReplayTime <= 0 || Observed->SampleIndex != Expected || !Observed->GetActorLocation().Equals(SamplePosition(Expected), 1.0))
			{
				return false;
			}

			Probe = Observed;
			Sample = Expected;
			SampleStarted = LastReplayTime;
			Test->AddInfo(FString::Printf(TEXT("PlaybackSample Index=%d Time=%.3f Position=%s Role=%d"), Sample, LastReplayTime,
				*Observed->GetActorLocation().ToString(), int32(Observed->GetLocalRole())));
			return false;
		}

		void Stop()
		{
			CompletedReplayTime = LastReplayTime;
			GameInstance->StopRecordingReplay();
			bStopping = true;
		}

		bool Fail(const TCHAR* Reason)
		{
			UWorld* World = GameInstance.IsValid() ? GameInstance->GetWorld() : nullptr;
			UDemoNetDriver* Driver = World ? World->GetDemoNetDriver() : nullptr;
			AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
			if (Driver && Settings)
			{
				int32 ProbeCount = 0;
				for (TActorIterator<AReplayStateProbe> It(World); It; ++It)
				{
					++ProbeCount;
				}

				Test->AddInfo(FString::Printf(TEXT("ReplayDiagnostic Frame=%d Total=%.3f WorldTime=%.3f Delta=%.6f Dilation=%.3f Paused=%d BegunPlay=%d Probes=%d Packets=%d Task=%d"),
					Driver->GetDemoFrameNum(), Driver->GetDemoTotalTime(), World->GetTimeSeconds(), World->GetDeltaSeconds(), Settings->DemoPlayTimeDilation,
					Settings->GetPauserPlayerState() != nullptr, World->HasBegunPlay(), ProbeCount, Driver->PlaybackPackets.Num(), Driver->IsAnyTaskPending()));
			}

			Test->AddError(FString::Printf(TEXT("NativeReplayFailed Reason=%s Playback=%d Initialized=%d Stopping=%d Sample=%d Time=%.3f"),
				Reason, bPlayback, bInitialized, bStopping, Sample, LastReplayTime));
			Cleanup();
			return true;
		}

		void Cleanup()
		{
			if (UGameInstance* Instance = GameInstance.Get())
			{
				if (bOwnReplay)
				{
					Instance->StopRecordingReplay();
					bOwnReplay = false;
				}

				if (bInitialized)
				{
					if (UReplaySubsystem* Replay = Instance->GetSubsystem<UReplaySubsystem>())
					{
						Replay->bLoadDefaultMapOnStop = bOriginalLoadDefaultMapOnStop;
					}

					bInitialized = false;
				}
			}

			if (!bPlayback && Probe.IsValid())
			{
				Probe->Destroy();
			}

			Probe.Reset();
		}


	private:
		FAutomationTestBase* Test;
		bool bPlayback;
		FString Directory;
		double Started;
		const float SampleDuration;
		double LastAdvanceWallSeconds;

		TWeakObjectPtr<UGameInstance> GameInstance;
		TWeakObjectPtr<AReplayStateProbe> Probe;
		bool bInitialized = false;
		bool bOwnReplay = false;
		bool bOriginalLoadDefaultMapOnStop = true;
		bool bStopping = false;
		bool bActiveObserved = false;

		int32 Sample = INDEX_NONE;
		float LastReplayTime = 0;
		float PreviousReplayTime = 0;
		float SampleStarted = 0;
		float CompletedReplayTime = 0;
	};

	bool EnqueueScenario(FAutomationTestBase* Test, bool bPlayback)
	{
		FString Run;
		FGuid RunId;
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaReplayFixture")) || !FParse::Value(FCommandLine::Get(), TEXT("MobaReplayRun="), Run)
			|| !FGuid::ParseExact(Run, EGuidFormats::Digits, RunId) || !RunId.IsValid())
		{
			Test->AddError(TEXT("该测试仅用于独立进程：需要 -MobaReplayFixture -MobaReplayRun=<32位GUID>，见 Docs/Replay.md。"));
			return false;
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("AI/TASK-015/Replay"), RunId.ToString(EGuidFormats::Digits)));
		// 只改变输入录制时长，保留三秒失败对照；不开放任意时长或修改播放时间/断言。
		FString HoldSeconds = TEXT("3");
		FParse::Value(FCommandLine::Get(), TEXT("MobaReplaySampleSeconds="), HoldSeconds);
		if (HoldSeconds != TEXT("1") && HoldSeconds != TEXT("3"))
		{
			Test->AddError(TEXT("MobaReplaySampleSeconds 仅允许 1（原始对照）或 3（超过五秒预读窗口的输入）。"));
			return false;
		}

		const float SampleDuration = HoldSeconds == TEXT("3") ? 3.0f : 1.0f;
		Test->AddInfo(FString::Printf(TEXT("NativeReplayInput Playback=%d HoldSeconds=%.0f Samples=%d"), bPlayback, SampleDuration, SampleCount));
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FNativeReplayScenario>(Test, bPlayback, Directory, SampleDuration));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeReplayRecordTest, "Moba.Replay.NativeActor.Record", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FNativeReplayRecordTest::RunTest(const FString& Parameters)
{
	return EnqueueScenario(this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeReplayPlaybackTest, "Moba.Replay.NativeActor.Playback", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FNativeReplayPlaybackTest::RunTest(const FString& Parameters)
{
	return EnqueueScenario(this, true);
}

#endif
