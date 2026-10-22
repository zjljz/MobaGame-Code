#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Hero/HeroDefinition.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFeaturesSubsystem.h"
#include "GameplayEffect.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Hero/HeroCharacter.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Match/ParticipantState.h"
#include "ReplaySubsystem.h"
#include "String/LexFromString.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	const TCHAR* HeroReplayName = TEXT("PublicHero");
	const TCHAR* HeroAssetPath = TEXT("/MobaClassic/Definitions/Hero_FrameworkTest.Hero_FrameworkTest");
	const FVector HeroSpawnPosition(100, -200, 300);
	const FVector HeroMovedPosition(600, 250, 300);
	constexpr float HeroSampleDuration = 3.0f;

	enum class EHeroCleanupProbe : uint8
	{
		None,
		Timeout,
		Destructor
	};

	struct FHeroCleanupEvidence
	{
		int32 ReleaseCalls = 0;
		bool bTimeoutObserved = false;
		bool bLeaseRetained = false;
		bool bSuccessReported = false;
	};

	class FHeroReplayScenario final : public IAutomationLatentCommand
	{
	public:
		FHeroReplayScenario(FAutomationTestBase* InTest, bool bInPlayback, FString InDirectory,
			EHeroCleanupProbe InCleanupProbe = EHeroCleanupProbe::None, TSharedPtr<FHeroCleanupEvidence> InCleanupEvidence = nullptr)
			: Test(InTest), bPlayback(bInPlayback), Directory(MoveTemp(InDirectory)), Started(FPlatformTime::Seconds()), LastAdvance(Started),
			CleanupProbe(InCleanupProbe), CleanupEvidence(MoveTemp(InCleanupEvidence))
		{
		}

		virtual ~FHeroReplayScenario() override
		{
			StopOwnedReplay();
			ReleaseFeature();
			RestoreReplaySetting();
		}

		virtual bool Update() override
		{
			if (bCleaning)
			{
				return AdvanceCleanup();
			}

			if (FPlatformTime::Seconds() - Started > 60.0)
			{
				return Fail(TEXT("ScenarioTimeout"));
			}

			if (!bReplayRequested)
			{
				return Prepare();
			}

			UWorld* World = Instance.IsValid() ? Instance->GetWorld() : nullptr;
			UReplaySubsystem* Replay = Instance.IsValid() ? Instance->GetSubsystem<UReplaySubsystem>() : nullptr;
			UDemoNetDriver* Driver = World ? World->GetDemoNetDriver() : nullptr;
			if (!Replay || !World)
			{
				return Fail(TEXT("InstanceOrWorldLost"));
			}

			if (!Driver || (bPlayback ? !Replay->IsPlaying() : !Replay->IsRecording()))
			{
				return bActive ? Fail(TEXT("ActiveReplayLost")) : false;
			}

			const float CurrentTime = Replay->GetReplayCurrentTime();
			if (CurrentTime > ReplayTime)
			{
				LastAdvance = FPlatformTime::Seconds();
			}

			if (bActive && CurrentTime < ReplayTime)
			{
				return Fail(TEXT("ReplayTimeRegressed"));
			}

			ReplayTime = CurrentTime;
			if (FPlatformTime::Seconds() - LastAdvance > 5.0)
			{
				return Fail(TEXT("ReplayClockStalled"));
			}

			if (!World->HasBegunPlay() || ReplayTime <= 0 || Replay->GetActiveReplayName().IsEmpty())
			{
				return false;
			}

			if (Replay->GetActiveReplayName() != HeroReplayName)
			{
				return Fail(TEXT("ReplayIdentityMismatch"));
			}

			bActive = true;
			return bPlayback ? Observe(World) : Record();
		}


	public:
		bool IsReadyForCleanupProbe() const { return bActive && Sample == 0 && Hero.IsValid() && Participant.IsValid(); }

		void StartCleanupProbe() { BeginCleanup(); }

		TWeakObjectPtr<UGameInstance> GetInstance() const { return Instance; }
		TWeakObjectPtr<AHeroCharacter> GetHero() const { return Hero; }
		TWeakObjectPtr<AParticipantState> GetParticipant() const { return Participant; }
		TWeakObjectPtr<UExperienceFeatureSubsystem> GetFeatures() const { return Features; }
		const FString& GetFeatureURL() const { return FeatureURL; }
		const FGuid& GetLease() const { return Lease; }


	private:
		bool Prepare()
		{
			if (!Instance.IsValid())
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

				AGameModeBase* Mode = World->GetAuthGameMode();
				UGameInstance* GameInstance = World->GetGameInstance();
				if (World->GetNetMode() != NM_Standalone || World->GetOutermost()->GetName() != TEXT("/Engine/Maps/Entry") || !Mode
					|| Mode->GetClass() != AGameModeBase::StaticClass() || !GameInstance || GameInstance->GetClass() != UGameInstance::StaticClass())
				{
					return Fail(TEXT("RequiresNativeEntryFixture"));
				}

				for (TActorIterator<APlayerController> It(World); It; ++It)
				{
					if (It->GetClass() != APlayerController::StaticClass())
					{
						return Fail(TEXT("ProjectControllerNotAllowed"));
					}
				}

				for (TActorIterator<AHeroCharacter> It(World); It; ++It)
				{
					return Fail(TEXT("HeroAlreadyPresent"));
				}

				Instance = GameInstance;
				Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
				if (CleanupProbe != EHeroCleanupProbe::None)
				{
					FString ProbeURL;
					if (!Features.IsValid() || !UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), ProbeURL)
						|| Features->GetLeaseCount(ProbeURL) != 0 || Features->GetState(ProbeURL) != EExperienceFeatureState::Absent)
					{
						return Fail(TEXT("CleanupProbeRequiresUnleasedAbsentFeature"));
					}

					Test->AddInfo(TEXT("HeroCleanupProbe BeforeAcquire LeaseCount=0 State=Absent"));
				}

				Lease = FGuid::NewGuid();
				if (!Features.IsValid() || !Features->Acquire(TEXT("MobaClassic"), Lease, FeatureURL))
				{
					Lease.Invalidate();
					return Fail(TEXT("FeatureAcquireFailed"));
				}

				if (CleanupProbe != EHeroCleanupProbe::None)
				{
					if (Features->GetLeaseCount(FeatureURL) != 1)
					{
						return Fail(TEXT("CleanupProbeAcquireNotExclusive"));
					}

					Test->AddInfo(TEXT("HeroCleanupProbe AfterAcquire LeaseCount=1"));
				}
			}

			if (!Features.IsValid() || Features->GetState(FeatureURL) == EExperienceFeatureState::Failed)
			{
				return Fail(TEXT("FeatureLoadFailed"));
			}

			if (Features->GetState(FeatureURL) != EExperienceFeatureState::Loaded)
			{
				return false;
			}

			Definition.Reset(LoadObject<UHeroDefinition>(nullptr, HeroAssetPath));
			if (!Definition.IsValid() || Definition->MaxHealth <= 0 || Definition->RespawnDelay < 1 || Definition->RespawnDelay > 20
				|| Definition->RespawnHealthFraction <= 0 || Definition->RespawnHealthFraction > 1)
			{
				return Fail(TEXT("SavedDefinitionOutsideFixtureContract"));
			}

			AbilitySet.Reset(Definition->AbilitySet.LoadSynchronous());
			if (!AbilitySet.IsValid() || AbilitySet->Abilities.IsEmpty())
			{
				return Fail(TEXT("SavedAbilitySetUnavailable"));
			}

			for (const FAbilityGrantDefinition& Grant : AbilitySet->Abilities)
			{
				UClass* Class = Grant.Ability.LoadSynchronous();
				if (!Class)
				{
					return Fail(TEXT("SavedAbilityClassUnavailable"));
				}

				LoadedClasses.Emplace(Class);
			}

			for (const auto& Effect : AbilitySet->Effects)
			{
				UClass* Class = Effect.LoadSynchronous();
				if (!Class)
				{
					return Fail(TEXT("SavedEffectClassUnavailable"));
				}

				LoadedClasses.Emplace(Class);
			}

			UWorld* World = Instance->GetWorld();
			UReplaySubsystem* Replay = Instance->GetSubsystem<UReplaySubsystem>();
			if (!Replay || Replay->IsRecording() || Replay->IsPlaying() || World->GetDemoNetDriver())
			{
				return Fail(TEXT("ReplaySubsystemBusy"));
			}

			const FString Receipt = FPaths::Combine(Directory, TEXT("Record.complete"));
			const FString Demo = FPaths::Combine(Directory, TEXT("PublicHero.replay"));
			if (bPlayback)
			{
				TArray<FString> Lines;
				uint32 RecorderPID = 0;
				if (!FFileHelper::LoadFileToStringArray(Lines, *Receipt) || Lines.Num() != 2 || !LexTryParseString(RecorderPID, *Lines[0]) || RecorderPID == 0
					|| RecorderPID == FPlatformProcess::GetCurrentProcessId() || !FGuid::Parse(Lines[1], Entity) || !Entity.IsValid() || IFileManager::Get().FileSize(*Demo) <= 0)
				{
					return Fail(TEXT("RequiresIndependentCompletedHeroRecording"));
				}
			}
			else
			{
				if (IFileManager::Get().FileExists(*Receipt) || IFileManager::Get().FileExists(*Demo))
				{
					return Fail(TEXT("RunAlreadyExists"));
				}

				// 在首次生成前保守记录；初始化失败或弱引用尚未建立也不能提前卸载内容。
				bWorldReferencesMayExist = true;
				Participant = World->SpawnActor<AParticipantState>();
				if (!Participant.IsValid())
				{
					return Fail(TEXT("ParticipantSpawnFailed"));
				}

				// 仅为现有初始化API提供公开身份；没有项目Controller、准入连接或凭证。
				Participant->ParticipantId = FGuid::NewGuid();
				Participant->MatchId = FGuid::NewGuid();
				Participant->TeamId = 7;
				Participant->bConnected = false;
				FActorSpawnParameters Spawn;
				Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), HeroSpawnPosition, FRotator::ZeroRotator, Spawn);
				if (!Hero.IsValid() || !Hero->InitializeHero(Definition.Get(), Participant.Get()))
				{
					return Fail(TEXT("HeroInitializationFailed"));
				}

				Participant->PrimaryHero = Hero.Get();
				Hero->GetCharacterMovement()->DisableMovement();
				Entity = Hero->EntityId;
			}

			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				return Fail(TEXT("OutputDirectoryUnavailable"));
			}

			bOriginalLoadDefaultMap = Replay->bLoadDefaultMapOnStop;
			Replay->bLoadDefaultMapOnStop = false;
			bSettingChanged = true;
			bReplayRequested = true;
			bOwnReplay = true;
			LastAdvance = FPlatformTime::Seconds();
			const TArray<FString> Options = {TEXT("ReplayStreamerOverride=LocalFileNetworkReplayStreaming"), TEXT("ReplayStreamerDemoPath=") + Directory + TEXT("/")};
			Test->AddInfo(FString::Printf(TEXT("HeroReplayInput Playback=%d MaxHealth=%.3f RespawnDelay=%.3f RespawnFraction=%.3f"), bPlayback,
				Definition->MaxHealth, Definition->RespawnDelay, Definition->RespawnHealthFraction));
			if (bPlayback)
			{
				// PlayReplay可能异步恢复尚未被本测试观察到的对象。
				bWorldReferencesMayExist = true;
				if (!Instance->PlayReplay(HeroReplayName, nullptr, Options))
				{
					return Fail(TEXT("PlayReplayRejected"));
				}
			}
			else
			{
				Instance->StartRecordingReplay(HeroReplayName, TEXT("TASK-015 public Hero fixture"), Options);
			}

			return false;
		}

		bool Matches(const AHeroCharacter* Candidate, int32 Expected) const
		{
			const FHeroLifeSnapshot& Life = Candidate->GetLifeSnapshot();
			const bool bRespawned = Expected == 3;
			const FVector Position = Expected == 0 || bRespawned ? HeroSpawnPosition : HeroMovedPosition;
			const float Health = Expected == 2 ? 0 : Definition->MaxHealth * (Expected == 1 ? 0.5f : bRespawned ? Definition->RespawnHealthFraction : 1.0f);
			return Candidate->EntityId == Entity && Candidate->InitializationGeneration == 1 && Candidate->GetDefinition() == Definition.Get()
				&& Life.State == (Expected == 2 ? EHeroLifeState::Dead : EHeroLifeState::Alive) && Life.Generation == (bRespawned ? 2u : 1u)
				&& Life.DeathCount == (Expected >= 2 ? 1u : 0u) && FMath::IsNearlyEqual(Candidate->GetAttributes()->GetHealth(), Health, 0.01f)
				&& Candidate->GetActorLocation().Equals(Position, 1.0);
		}

		/**
		 * 只在录制权威端用原生Instant GE准备伤害状态；死亡/复活由现有Hero和AttributeSet完成。
		 */
		void ApplyDamage(float Amount)
		{
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(Instance->GetWorld());
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			auto& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = UHeroAttributeSet::GetIncomingDamageAttribute();
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(Amount);
			UHeroAbilitySystemComponent* Abilities = Hero->GetHeroAbilitySystem();
			Abilities->ApplyGameplayEffectToSelf(Effect, 1, Abilities->MakeEffectContext());
			Hero->ForceNetUpdate();
		}

		bool Record()
		{
			if (!Hero.IsValid() || !Hero->HasAuthority() || Hero->EntityId != Entity || Hero->InitializationGeneration != 1
				|| Hero->GetHeroAbilitySystem()->GetSourceCount() != 1)
			{
				return Fail(TEXT("RecordingHeroIdentityOrGrantChanged"));
			}

			if (Sample == INDEX_NONE)
			{
				if (!Matches(Hero.Get(), 0))
				{
					return Fail(TEXT("InitialHeroStateMismatch"));
				}

				AcceptSample(Hero.Get(), 0);
			}
			else if (Sample == 0 && ReplayTime - SampleStarted >= HeroSampleDuration)
			{
				Hero->SetActorLocation(HeroMovedPosition, false, nullptr, ETeleportType::TeleportPhysics);
				ApplyDamage(Definition->MaxHealth * 0.5f);
				if (!Matches(Hero.Get(), 1))
				{
					return Fail(TEXT("MovedDamagedHeroMismatch"));
				}

				AcceptSample(Hero.Get(), 1);
			}
			else if (Sample == 1 && ReplayTime - SampleStarted >= HeroSampleDuration)
			{
				ApplyDamage(Definition->MaxHealth);
				if (!Matches(Hero.Get(), 2))
				{
					return Fail(TEXT("NativeDeathNotCommitted"));
				}

				AcceptSample(Hero.Get(), 2);
			}
			else if (Sample == 2 && Matches(Hero.Get(), 3))
			{
				Hero->GetCharacterMovement()->DisableMovement();
				Hero->ForceNetUpdate();
				AcceptSample(Hero.Get(), 3);
			}
			else if (Sample == 3 && ReplayTime - SampleStarted >= HeroSampleDuration)
			{
				StopOwnedReplay();
				UReplaySubsystem* Replay = Instance->GetSubsystem<UReplaySubsystem>();
				if (Replay->IsRecording() || Replay->IsPlaying() || Instance->GetWorld()->GetDemoNetDriver())
				{
					return Fail(TEXT("RecordStopNotObserved"));
				}

				const FString Receipt = FString::Printf(TEXT("%u\n%s\n"), FPlatformProcess::GetCurrentProcessId(), *Entity.ToString());
				if (!FFileHelper::SaveStringToFile(Receipt, *FPaths::Combine(Directory, TEXT("Record.complete")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return Fail(TEXT("ReceiptWriteFailed"));
				}

				BeginCleanup();
			}

			return false;
		}

		bool Observe(UWorld* World)
		{
			AHeroCharacter* Observed = nullptr;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (Observed)
				{
					return Fail(TEXT("DuplicatePlaybackHero"));
				}

				Observed = *It;
			}

			if (!Observed)
			{
				return false;
			}

			if (Observed->HasAuthority() || (Sample != INDEX_NONE && Hero.Get() != Observed))
			{
				return Fail(TEXT("PlaybackMustRetainSameNonAuthorityHero"));
			}

			if (Sample == 3)
			{
				if (ReplayTime - SampleStarted >= 0.25f)
				{
					StopOwnedReplay();
					BeginCleanup();
				}

				return false;
			}

			if (Matches(Observed, Sample + 1))
			{
				Hero = Observed;
				AcceptSample(Observed, Sample + 1);
			}

			return false;
		}

		void AcceptSample(const AHeroCharacter* Observed, int32 Index)
		{
			Sample = Index;
			SampleStarted = ReplayTime;
			const FHeroLifeSnapshot& Life = Observed->GetLifeSnapshot();
			Test->AddInfo(FString::Printf(TEXT("Hero%sSample Index=%d Time=%.3f Health=%.3f Life=%d Generation=%u Deaths=%u Position=%s Role=%d Entity=%s Init=%u"),
				bPlayback ? TEXT("Playback") : TEXT("Record"), Sample, ReplayTime, Observed->GetAttributes()->GetHealth(), int32(Life.State), Life.Generation,
				Life.DeathCount, *Observed->GetActorLocation().ToString(), int32(Observed->GetLocalRole()), *Observed->EntityId.ToString(), Observed->InitializationGeneration));
		}

		bool Fail(const TCHAR* Reason)
		{
			UWorld* World = Instance.IsValid() ? Instance->GetWorld() : nullptr;
			if (World)
			{
				for (TActorIterator<AHeroCharacter> It(World); It; ++It)
				{
					const FHeroLifeSnapshot& Life = It->GetLifeSnapshot();
					Test->AddInfo(FString::Printf(TEXT("HeroReplayDiagnostic Health=%.3f Life=%d Generation=%u Deaths=%u Position=%s Role=%d Init=%u DefinitionReady=%d"),
						It->GetAttributes()->GetHealth(), int32(Life.State), Life.Generation, Life.DeathCount, *It->GetActorLocation().ToString(),
						int32(It->GetLocalRole()), It->InitializationGeneration, It->GetDefinition() == Definition.Get()));
					break;
				}
			}

			Test->AddError(FString::Printf(TEXT("HeroReplayFailed Reason=%s Playback=%d Sample=%d Time=%.3f Hero=%d"), Reason, bPlayback, Sample, ReplayTime, Hero.IsValid()));
			bFailed = true;
			BeginCleanup();
			return false;
		}

		void StopOwnedReplay()
		{
			if (bOwnReplay && Instance.IsValid())
			{
				Instance->StopRecordingReplay();
			}

			bOwnReplay = false;
		}

		void BeginCleanup()
		{
			StopOwnedReplay();
			bCleaning = true;
			CleanupStarted = FPlatformTime::Seconds();
			if (Instance.IsValid() && Instance->GetWorld() && (bReplayRequested || Hero.IsValid() || Participant.IsValid()))
			{
				CleanupWorld = Instance->GetWorld();
				bCleanupWorldCaptured = true;
				// 正常travel先销毁录制/回放对象，再释放内容租约，避免存活Hero引用已卸载Feature。
				if (CleanupProbe != EHeroCleanupProbe::Timeout)
				{
					UGameplayStatics::OpenLevel(Instance.Get(), FName(TEXT("/Engine/Maps/Entry")), true, TEXT("game=/Script/Engine.GameModeBase"));
				}
			}
			else
			{
				ReleaseFeature();
			}
		}

		bool AdvanceCleanup()
		{
			if (FPlatformTime::Seconds() - CleanupStarted > 15.0)
			{
				Test->AddError(TEXT("HeroReplayCleanupTimeout：未把清理失败计作场景通过。"));
				if (CleanupEvidence)
				{
					CleanupEvidence->bTimeoutObserved = true;
				}

				ReleaseFeature();
				RestoreReplaySetting();
				return true;
			}

			if (Lease.IsValid())
			{
				if (!CanReleaseFeature())
				{
					return false;
				}

				ReleaseFeature();
			}

			if (Features.IsValid() && !FeatureURL.IsEmpty() && Features->GetState(FeatureURL) != EExperienceFeatureState::Absent)
			{
				return false;
			}

			RestoreReplaySetting();
			if (!bFailed)
			{
				if (CleanupEvidence)
				{
					CleanupEvidence->bSuccessReported = true;
				}

				Test->AddInfo(FString::Printf(TEXT("HeroReplayComplete Playback=%d Samples=4 Time=%.3f PID=%u FeatureReleased=1"), bPlayback, ReplayTime, FPlatformProcess::GetCurrentProcessId()));
			}

			return true;
		}

		// 停播不等于旧对象已清理；没有完整travel证据时保留最后租约到隔离进程退出。
		bool CanReleaseFeature() const
		{
			if (!bWorldReferencesMayExist)
			{
				return true;
			}

			UWorld* World = Instance.IsValid() ? Instance->GetWorld() : nullptr;
			UReplaySubsystem* Replay = Instance.IsValid() ? Instance->GetSubsystem<UReplaySubsystem>() : nullptr;
			return bCleanupWorldCaptured && !CleanupWorld.IsValid() && !Hero.IsValid() && !Participant.IsValid()
				&& World && World->HasBegunPlay() && Replay && !Replay->IsRecording() && !Replay->IsPlaying() && !World->GetDemoNetDriver();
		}

		void ReleaseFeature()
		{
			if (Lease.IsValid() && !CanReleaseFeature())
			{
				if (CleanupEvidence)
				{
					CleanupEvidence->bLeaseRetained = true;
				}

				Test->AddInfo(FString::Printf(TEXT("HeroReplayLeaseRetained Reason=UnconfirmedWorldCleanup LeaseCount=%d Until=IsolatedProcessExit"),
					Features.IsValid() ? Features->GetLeaseCount(FeatureURL) : INDEX_NONE));
				return;
			}

			LoadedClasses.Empty();
			AbilitySet.Reset();
			Definition.Reset();
			if (Lease.IsValid() && Features.IsValid())
			{
				if (CleanupEvidence)
				{
					++CleanupEvidence->ReleaseCalls;
				}

				Features->Release(FeatureURL, Lease);
			}

			Lease.Invalidate();
		}

		void RestoreReplaySetting()
		{
			if (bSettingChanged && Instance.IsValid())
			{
				if (UReplaySubsystem* Replay = Instance->GetSubsystem<UReplaySubsystem>())
				{
					Replay->bLoadDefaultMapOnStop = bOriginalLoadDefaultMap;
				}
			}

			bSettingChanged = false;
		}


	private:
		FAutomationTestBase* Test;
		bool bPlayback;
		FString Directory;
		double Started;
		double LastAdvance;
		double CleanupStarted = 0;
		EHeroCleanupProbe CleanupProbe;
		TSharedPtr<FHeroCleanupEvidence> CleanupEvidence;

		TWeakObjectPtr<UGameInstance> Instance;
		TWeakObjectPtr<UExperienceFeatureSubsystem> Features;
		TWeakObjectPtr<UWorld> CleanupWorld;
		TWeakObjectPtr<AHeroCharacter> Hero;
		TWeakObjectPtr<AParticipantState> Participant;
		TStrongObjectPtr<UHeroDefinition> Definition;
		TStrongObjectPtr<UAbilitySetDefinition> AbilitySet;
		TArray<TStrongObjectPtr<UClass>> LoadedClasses;
		FString FeatureURL;
		FGuid Lease;
		FGuid Entity;

		int32 Sample = INDEX_NONE;
		float ReplayTime = 0;
		float SampleStarted = 0;
		bool bReplayRequested = false;
		bool bOwnReplay = false;
		bool bActive = false;
		bool bFailed = false;
		bool bCleaning = false;
		bool bSettingChanged = false;
		bool bOriginalLoadDefaultMap = true;
		bool bWorldReferencesMayExist = false;
		bool bCleanupWorldCaptured = false;
	};

	// 观察者独立持有证据和弱引用，必须在Scenario真正析构后核对租约保护。
	class FHeroCleanupSafetyScenario final : public IAutomationLatentCommand
	{
	public:
		FHeroCleanupSafetyScenario(FAutomationTestBase* InTest, FString InDirectory, EHeroCleanupProbe InProbe)
			: Test(InTest), Probe(InProbe), Evidence(MakeShared<FHeroCleanupEvidence>()),
			Scenario(MakeShared<FHeroReplayScenario>(InTest, false, MoveTemp(InDirectory), InProbe, Evidence))
		{
		}

		virtual bool Update() override
		{
			if (!bCaptured)
			{
				if (Scenario->Update())
				{
					Test->AddError(TEXT("HeroCleanupProbe SetupEndedBeforeInitialSample"));
					Scenario.Reset();
					return true;
				}

				if (!Scenario->IsReadyForCleanupProbe())
				{
					return false;
				}

				Instance = Scenario->GetInstance();
				OldWorld = Instance->GetWorld();
				Hero = Scenario->GetHero();
				Participant = Scenario->GetParticipant();
				Features = Scenario->GetFeatures();
				FeatureURL = Scenario->GetFeatureURL();
				Lease = Scenario->GetLease();
				bCaptured = true;
				if (Probe == EHeroCleanupProbe::Timeout)
				{
					Scenario->StartCleanupProbe();
					return false;
				}
			}
			else if (Probe == EHeroCleanupProbe::Timeout && !Scenario->Update())
			{
				return false;
			}

			// 不能只检查析构前的计数：原缺陷恰好发生在Scenario析构时。
			Scenario.Reset();
			Test->TestTrue(TEXT("负向场景对象已析构"), !Scenario.IsValid());
			Test->TestTrue(TEXT("旧World仍有效且没有被替换"), OldWorld.IsValid() && Instance.IsValid() && Instance->GetWorld() == OldWorld.Get());
			Test->TestTrue(TEXT("真实初始化Hero与Participant仍有效"), Hero.IsValid() && Participant.IsValid()
				&& Hero->InitializationGeneration == 1 && Hero->GetDefinition() != nullptr);
			Test->TestTrue(TEXT("捕获的本测试Lease有效"), Lease.IsValid());
			Test->TestTrue(TEXT("Feature观察者有效"), Features.IsValid());
			if (Features.IsValid())
			{
				Test->TestEqual(TEXT("析构后最后一个租约仍保留"), Features->GetLeaseCount(FeatureURL), 1);
				Test->TestEqual(TEXT("析构后Feature仍Loaded"), Features->GetState(FeatureURL), EExperienceFeatureState::Loaded);
			}

			Test->TestEqual(TEXT("没有调用Feature Release"), Evidence->ReleaseCalls, 0);
			Test->TestTrue(TEXT("实际经过保留租约保护"), Evidence->bLeaseRetained);
			Test->TestFalse(TEXT("没有报告普通场景成功完成"), Evidence->bSuccessReported);
			Test->TestEqual(TEXT("只有Timeout用例观察到清理超时"), Evidence->bTimeoutObserved, Probe == EHeroCleanupProbe::Timeout);
			UReplaySubsystem* Replay = Instance.IsValid() ? Instance->GetSubsystem<UReplaySubsystem>() : nullptr;
			Test->TestTrue(TEXT("析构仍停止自有Replay"), Replay && !Replay->IsRecording() && !Replay->IsPlaying()
				&& OldWorld.IsValid() && !OldWorld->GetDemoNetDriver());
			Test->AddInfo(FString::Printf(TEXT("HeroCleanupSafetyComplete Mode=%s ScenarioDestroyed=1 OldWorld=%d Hero=%d LeaseCount=%d State=%d ")
				TEXT("ReleaseCalls=%d SuccessReported=%d Timeout=%d Until=IsolatedProcessExit PID=%u"),
				Probe == EHeroCleanupProbe::Timeout ? TEXT("Timeout") : TEXT("Destructor"), OldWorld.IsValid(), Hero.IsValid(),
				Features.IsValid() ? Features->GetLeaseCount(FeatureURL) : INDEX_NONE,
				Features.IsValid() ? int32(Features->GetState(FeatureURL)) : INDEX_NONE, Evidence->ReleaseCalls,
				Evidence->bSuccessReported, Evidence->bTimeoutObserved, FPlatformProcess::GetCurrentProcessId()));
			// 故意不释放租约：本用例独立进程退出才结束残留对象与Feature的隔离边界。
			return true;
		}


	private:
		FAutomationTestBase* Test;
		EHeroCleanupProbe Probe;
		TSharedPtr<FHeroCleanupEvidence> Evidence;
		TSharedPtr<FHeroReplayScenario> Scenario;
		TWeakObjectPtr<UGameInstance> Instance;
		TWeakObjectPtr<UWorld> OldWorld;
		TWeakObjectPtr<AHeroCharacter> Hero;
		TWeakObjectPtr<AParticipantState> Participant;
		TWeakObjectPtr<UExperienceFeatureSubsystem> Features;
		FString FeatureURL;
		FGuid Lease;
		bool bCaptured = false;
	};

	bool EnqueueCleanupSafety(FAutomationTestBase* Test, EHeroCleanupProbe Probe)
	{
		FString Run;
		FGuid RunId;
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaHeroReplayCleanupSafetyFixture")) || !FParse::Value(FCommandLine::Get(), TEXT("MobaReplayRun="), Run)
			|| !FGuid::ParseExact(Run, EGuidFormats::Digits, RunId) || !RunId.IsValid())
		{
			Test->AddError(TEXT("CleanupSafety需要独立进程与 -MobaHeroReplayCleanupSafetyFixture -MobaReplayRun=<新32位GUID>。"));
			return false;
		}

		if (Probe == EHeroCleanupProbe::Timeout)
		{
			Test->AddExpectedError(TEXT("HeroReplayCleanupTimeout：未把清理失败计作场景通过。"), EAutomationExpectedErrorFlags::Contains, 1);
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("AI/TASK-015/CleanupSafety"), RunId.ToString(EGuidFormats::Digits)));
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FHeroCleanupSafetyScenario>(Test, Directory, Probe));
		return true;
	}

	bool EnqueueHeroReplay(FAutomationTestBase* Test, bool bPlayback)
	{
		FString Run;
		FGuid RunId;
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaHeroReplayFixture")) || !FParse::Value(FCommandLine::Get(), TEXT("MobaReplayRun="), Run)
			|| !FGuid::ParseExact(Run, EGuidFormats::Digits, RunId) || !RunId.IsValid())
		{
			Test->AddError(TEXT("Hero Replay需要独立进程与 -MobaHeroReplayFixture -MobaReplayRun=<32位GUID>。"));
			return false;
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("AI/TASK-015/HeroReplay"), RunId.ToString(EGuidFormats::Digits)));
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FHeroReplayScenario>(Test, bPlayback, Directory));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroReplayRecordTest, "Moba.Replay.HeroPublic.Record", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FHeroReplayRecordTest::RunTest(const FString& Parameters)
{
	return EnqueueHeroReplay(this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroReplayPlaybackTest, "Moba.Replay.HeroPublic.Playback", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FHeroReplayPlaybackTest::RunTest(const FString& Parameters)
{
	return EnqueueHeroReplay(this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroReplayCleanupTimeoutTest, "Moba.Replay.HeroPublic.CleanupSafety.Timeout", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FHeroReplayCleanupTimeoutTest::RunTest(const FString& Parameters)
{
	return EnqueueCleanupSafety(this, EHeroCleanupProbe::Timeout);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroReplayCleanupDestructorTest, "Moba.Replay.HeroPublic.CleanupSafety.Destructor",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FHeroReplayCleanupDestructorTest::RunTest(const FString& Parameters)
{
	return EnqueueCleanupSafety(this, EHeroCleanupProbe::Destructor);
}

#endif
