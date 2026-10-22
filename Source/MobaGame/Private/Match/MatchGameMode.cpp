#include "Match/MatchGameMode.h"

#include "Match/MatchGameState.h"
#include "Session/MobaSessionSubsystem.h"
#include "Session/GameFlowSubsystem.h"
#include "Engine/GameInstance.h"
#include "Match/ParticipantState.h"
#include "Player/HeroPlayerController.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Experience/ExperienceSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Experience/GameConfiguration.h"
#include "AIController.h"
#include "EngineUtils.h"
#include "Engine/LevelStreamingDynamic.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "MobaLogChannels.h"

namespace
{
	/**
	 * 核对预留参赛者仍属于当前比赛，且客户端确认的是当前内容实例。
	 * 仅检查上下文身份；预留期限、连接占用和内容就绪由提交入口分别检查。
	 */
	bool IsAdmissionContextCurrent(const AMatchGameState* MatchState, const AParticipantState* ParticipantState, const FGuid& ExperienceInstanceId)
	{
		return MatchState && ParticipantState && ParticipantState->MatchId == MatchState->MatchId && ExperienceInstanceId == MatchState->ExperienceInstanceId;
	}
}

AMatchGameMode::AMatchGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	DefaultPawnClass = nullptr;
	GameStateClass = AMatchGameState::StaticClass();
	PlayerStateClass = AParticipantState::StaticClass();
	PlayerControllerClass = AHeroPlayerController::StaticClass();
}

void AMatchGameMode::StartPlay()
{
	AMatchGameState* MatchState = GetGameState<AMatchGameState>();
	const UMobaAssetManager* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
	const UGameConfiguration* GameConfiguration = AssetManager ? AssetManager->GetGameConfiguration() : nullptr;
	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	FMatchLaunchSelection LaunchSelection;

	UGameFlowSubsystem* GameFlowSubsystem = GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
	const bool bLocalRequest = GameFlowSubsystem->ClaimLocalMatch(GetWorld(), LaunchSelection);
	const EInitialStartupDecision StartupDecision = bLocalRequest
		? EInitialStartupDecision::Match
		: GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->TryResolveInitialStartup(GetWorld(), GameConfiguration, LaunchSelection);

	// 比赛选择开放准入等待；实际提交仍须内容 Ready。Skip/Failure 均不启动内容。
	bMatchAdmissionOpen = (StartupDecision == EInitialStartupDecision::Match);
	if (StartupDecision == EInitialStartupDecision::Match)
	{
		MatchState->MatchId = FGuid::NewGuid();
		MatchState->ExperienceInstanceId = FGuid::NewGuid();
		GameFlowSubsystem->ObserveMatch(GetWorld());
		const EExperienceFailure StartResult = ExperienceSubsystem->StartExperience(LaunchSelection, MatchState->ExperienceInstanceId, true);

		UE_LOG(LogMobaControl, Display, TEXT("MatchStart Match=%s Instance=%s NetMode=%d ContentRequest=%s"),
		       *MatchState->MatchId.ToString(), *MatchState->ExperienceInstanceId.ToString(), int32(GetNetMode()), *UEnum::GetValueAsString(StartResult));
	}
	else if (StartupDecision == EInitialStartupDecision::Frontend || (StartupDecision == EInitialStartupDecision::Skip && GetNetMode() == NM_Standalone))
	{
		if (GetNetMode() == NM_Standalone)
		{
			GameFlowSubsystem->RequestFrontend();
		}
		else
		{
			ExperienceSubsystem->StartFrontend();
		}
	}

	Super::StartPlay();
}

void AMatchGameMode::PostLogin(APlayerController* NewPlayerController)
{
	Super::PostLogin(NewPlayerController);
	if (auto* HeroController = Cast<AHeroPlayerController>(NewPlayerController))
	{
		if (auto* ParticipantState = HeroController->GetPlayerState<AParticipantState>())
		{
			ParticipantState->bConnected = false;
		}

		if (!bMatchAdmissionOpen)
		{
			return;
		}

		PendingConnections.Add(HeroController, FPlatformTime::Seconds() + 75);
	}
}

void AMatchGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bMatchAdmissionOpen)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();
	TArray<TWeakObjectPtr<AHeroPlayerController>> ExpiredControllers;
	for (const auto& PendingConnection : PendingConnections)
	{
		if (!PendingConnection.Key.IsValid() || CurrentTime >= PendingConnection.Value)
		{
			ExpiredControllers.Add(PendingConnection.Key);
		}
	}

	for (const auto& HeroController : ExpiredControllers)
	{
		if (HeroController.IsValid())
		{
			ReleaseAdmission(HeroController.Get(), TEXT("Timeout"));
			HeroController->Destroy();
		}

		PendingConnections.Remove(HeroController);
		ContentPendingConnections.Remove(HeroController);
	}

	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	const EExperienceState ExperienceState = ExperienceSubsystem->GetSnapshot().State;
	const FExperienceSnapshot& ExperienceSnapshot = ExperienceSubsystem->GetSnapshot();
	AMatchGameState* MatchState = GetGameState<AMatchGameState>();
	if (!MatchState->Selection.ModeId.IsValid() && !ExperienceSnapshot.ModeConfigName.IsNone()
		&& (ExperienceState == EExperienceState::Loading || ExperienceState == EExperienceState::Activating || ExperienceState == EExperienceState::ExperienceReady))
	{
		// 名称解析后才复制完整选择；客户端不接收数组索引或“使用第一个”的本地开发政策。
		MatchState->Selection.ModeId = ExperienceSnapshot.ModeId;
		MatchState->Selection.ModeConfigName = ExperienceSnapshot.ModeConfigName;
		MatchState->ForceNetUpdate();
	}

	if (ExperienceState != EExperienceState::Loading && ExperienceState != EExperienceState::Activating)
	{
		// 先移交稳定副本；准入 RPC 可以同步回到服务器，不能遍历可重入修改的数组。
		const auto WaitingControllers = MoveTemp(ContentPendingConnections);
		ContentPendingConnections.Reset();
		for (const auto& WaitingController : WaitingControllers)
		{
			if (AHeroPlayerController* HeroController = WaitingController.Get(); HeroController && PendingConnections.Contains(HeroController))
			{
				BeginAdmission(HeroController, false, FResumeCredential());
			}
		}
	}

	if (!bMatchAdmissionOpen || ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		return;
	}

	if (MatchState->Teams.Teams.IsEmpty())
	{
		MatchState->Teams = ExperienceSubsystem->GetRules()->TeamRules;
	}

	for (AParticipantState* ParticipantState : Participants)
	{
		if (ParticipantState && ParticipantState->bConnected)
		{
			ParticipantState->MatchId = MatchState->MatchId;
			if (ParticipantState->TeamId == INDEX_NONE && !MatchState->Teams.Teams.IsEmpty())
			{
				ParticipantState->TeamId = MatchState->Teams.Teams[Participants.IndexOfByKey(ParticipantState) % MatchState->Teams.Teams.Num()];
			}

			TryPrepareHero(Cast<AHeroPlayerController>(ParticipantState->GetOwner()));
		}
	}
}

void AMatchGameMode::Logout(AController* ExitingController)
{
	DetachConnection(Cast<AHeroPlayerController>(ExitingController));
	Super::Logout(ExitingController);
}

void AMatchGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FinishMatch();
	Super::EndPlay(EndPlayReason);
}

void AMatchGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayerController)
{
	// 内容、身份与复制屏障接管生成时机；不得调用默认即时 RestartPlayer。
}

bool AMatchGameMode::OwnsParticipant(const AParticipantState* ParticipantState) const
{
	return Participants.Contains(ParticipantState);
}

void AMatchGameMode::DetachConnection(AHeroPlayerController* HeroController)
{
	if (!HeroController || GetWorld()->bIsTearingDown)
	{
		return;
	}

	ReleaseAdmission(HeroController, TEXT("Disconnected"));
	AParticipantState* ParticipantState = HeroController->GetPlayerState<AParticipantState>();
	if (!ParticipantState || !OwnsParticipant(ParticipantState) || !ParticipantState->bConnected || ParticipantState->GetOwner() != HeroController)
	{
		return;
	}

	ParticipantState->bConnected = false;
	++ParticipantState->ConnectionEpoch;
	AHeroCharacter* HeroCharacter = ParticipantState->PrimaryHero;
	if (HeroCharacter)
	{
		HeroCharacter->bInputReady = false;
		HeroCharacter->ConnectionEpoch = ParticipantState->ConnectionEpoch;
		HeroCharacter->ConsumeMovementInputVector();
		HeroCharacter->GetCharacterMovement()->StopMovementImmediately();
		HeroController->UnPossess();
		FActorSpawnParameters SpawnParameters;
		AAIController* StandbyController = GetWorld()->SpawnActor<AAIController>(AAIController::StaticClass(), HeroCharacter->GetActorTransform(), SpawnParameters);
		if (StandbyController)
		{
			StandbyController->Possess(HeroCharacter);
		}

		HeroCharacter->RefreshActorInfo();
		HeroCharacter->ForceNetUpdate();
	}

	ParticipantState->SetOwner(nullptr);
	ParticipantState->ForceNetUpdate();
	UE_LOG(LogMobaControl, Display, TEXT("ParticipantDetached Participant=%s Entity=%s Epoch=%u RetainedPlayerState=%s RetainedASC=%s Sources=%d"), *ParticipantState->ParticipantId.ToString(),
	       HeroCharacter ? *HeroCharacter->EntityId.ToString() : TEXT("None"), ParticipantState->ConnectionEpoch, *ParticipantState->GetName(), HeroCharacter ? *HeroCharacter->GetHeroAbilitySystem()->GetPathName() : TEXT("None"),
	       HeroCharacter ? HeroCharacter->GetHeroAbilitySystem()->GetSourceCount() : 0);
}

void AMatchGameMode::TryPrepareHero(AHeroPlayerController* HeroController)
{
	if (!HeroController || !HeroController->HasContentReady())
	{
		return;
	}

	AParticipantState* ParticipantState = HeroController->GetPlayerState<AParticipantState>();
	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	if (!ParticipantState || !ParticipantState->bConnected || !OwnsParticipant(ParticipantState) || ParticipantState->PrimaryHero || !ParticipantState->MatchId.IsValid()
		|| ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		return;
	}

	const UMatchRulesDefinition* MatchRulesDef = ExperienceSubsystem->GetRules();
	if (!MatchRulesDef || !ExperienceSubsystem->GetDevelopmentHero())
	{
		return;
	}

	const ULevelStreamingDynamic* MapStreamingInstance = ExperienceSubsystem->GetMapInstance();
	const ULevel* ContentLevel = MapStreamingInstance ? MapStreamingInstance->GetLoadedLevel() : nullptr;
	if (!ContentLevel)
	{
		return;
	}

	TArray<APlayerStart*> PlayerStarts;
	// 出生点属于当前内容地图；宿主 Entry/Lobby 中的 PlayerStart 不能混入比赛候选。
	for (TActorIterator<APlayerStart> PlayerStartIterator(GetWorld()); PlayerStartIterator; ++PlayerStartIterator)
	{
		if (PlayerStartIterator->GetLevel() == ContentLevel)
		{
			PlayerStarts.Add(*PlayerStartIterator);
		}
	}

	PlayerStarts.Sort([](const APlayerStart& LeftPlayerStart, const APlayerStart& RightPlayerStart) { return LeftPlayerStart.PlayerStartTag.LexicalLess(RightPlayerStart.PlayerStartTag); });
	const int32 ParticipantIndex = Participants.IndexOfByKey(ParticipantState);
	if (!PlayerStarts.IsValidIndex(ParticipantIndex))
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = HeroController;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
	AHeroCharacter* HeroCharacter = GetWorld()->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), PlayerStarts[ParticipantIndex]->GetActorTransform(), SpawnParameters);
	if (!HeroCharacter)
	{
		return;
	}

	if (!HeroCharacter->InitializeHero(ExperienceSubsystem->GetDevelopmentHero(), ParticipantState))
	{
		HeroCharacter->Destroy();
		return;
	}

	ParticipantState->PrimaryHero = HeroCharacter;
	ParticipantState->InitializeEconomy(MatchRulesDef->ItemEconomyRules.InitialGold);
	HeroController->Possess(HeroCharacter);
	HeroController->ClientBindingState(HeroCharacter->EntityId, HeroCharacter->ConnectionEpoch, HeroCharacter->GetLifeSnapshot().Generation, HeroCharacter->GetLifeSnapshot().DeathCount);
	ParticipantState->ForceNetUpdate();
	HeroCharacter->ForceNetUpdate();
	UE_LOG(LogMobaControl, Display, TEXT("HeroCreated Participant=%s Entity=%s Epoch=%u Init=%u Revision=%u Sources=%d Team=%d Start=%s Position=%s"),
	       *ParticipantState->ParticipantId.ToString(),
	       *HeroCharacter->EntityId.ToString(), HeroCharacter->ConnectionEpoch, HeroCharacter->InitializationGeneration, HeroCharacter->RequiredLoadoutRevision,
	       HeroCharacter->GetHeroAbilitySystem()->GetSourceCount(), ParticipantState->TeamId,
	       *PlayerStarts[ParticipantIndex]->GetPathName(), *HeroCharacter->GetActorLocation().ToCompactString());
}

void AMatchGameMode::BeginAdmission(AHeroPlayerController* HeroController, bool bResume, const FResumeCredential& Credential)
{
	if (!IsValid(HeroController))
	{
		return;
	}

	auto RejectAdmission = [HeroController](FName Reason) { HeroController->ClientAdmissionResult(FGuid(), Reason); };
	if (!bMatchAdmissionOpen)
	{
		RejectAdmission(TEXT("MatchEnded"));
		return;
	}

	const double* PendingConnectionDeadline = PendingConnections.Find(HeroController);
	if (!PendingConnectionDeadline)
	{
		RejectAdmission(TEXT("AlreadyBound"));
		return;
	}

	const double ConnectionDeadline = *PendingConnectionDeadline;
	if (FPlatformTime::Seconds() >= ConnectionDeadline)
	{
		ContentPendingConnections.Remove(HeroController);
		RejectAdmission(TEXT("Timeout"));
		return;
	}

	if (ContentPendingConnections.Contains(HeroController))
	{
		// 原首次请求继续等待；重复 RPC 不改请求类型、不续期，也不重复派发状态。
		return;
	}

	// 一条连接一次在途请求，重复RPC不能延长预留寿命。
	for (const FSeat& ExistingSeat : Seats)
	{
		if (ExistingSeat.ReservedBy == HeroController)
		{
			// 已发出的可靠准入结果继续有效；不能以空 request 的拒绝清掉拥有端状态。
			return;
		}
	}

	AMatchGameState* MatchState = GetGameState<AMatchGameState>();
	if (!MatchState || !MatchState->MatchId.IsValid())
	{
		RejectAdmission(TEXT("MatchUnavailable"));
		return;
	}

	FSeat* SelectedSeat = nullptr;
	if (bResume)
	{
		if (!Credential.IsValid())
		{
			RejectAdmission(TEXT("CredentialInvalid"));
			return;
		}

		if (Credential.Match != MatchState->MatchId)
		{
			RejectAdmission(TEXT("WrongMatch"));
			return;
		}

		SelectedSeat = Seats.FindByPredicate([&](const FSeat& CandidateSeat)
		{
			return CandidateSeat.Participant.IsValid() && CandidateSeat.Participant->ParticipantId == Credential.Participant;
		});
		if (!SelectedSeat || !ResumeCredential::Matches(Credential.Secret, SelectedSeat->Digest))
		{
			RejectAdmission(TEXT("CredentialInvalid"));
			return;
		}

		if (SelectedSeat->Participant->bConnected)
		{
			RejectAdmission(TEXT("ConnectionActive"));
			return;
		}

		if (SelectedSeat->ReservedBy.IsValid())
		{
			RejectAdmission(TEXT("SeatReserved"));
			return;
		}

		if (!SelectedSeat->bCommitted || !IsValid(SelectedSeat->Participant->PrimaryHero))
		{
			RejectAdmission(TEXT("SeatUnavailable"));
			return;
		}
	}
	else
	{
		UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
		const EExperienceState ExperienceState = ExperienceSubsystem->GetSnapshot().State;
		if (ExperienceState == EExperienceState::Loading || ExperienceState == EExperienceState::Activating
			|| (ExperienceState == EExperienceState::ExperienceReady && !ContentPendingConnections.IsEmpty()))
		{
			// Ready 通知与 Tick 之间的新请求也排在旧等待者后，避免抢占最后席位。
			ContentPendingConnections.Add(HeroController);
			RejectAdmission(TEXT("ContentNotReady"));
			return;
		}

		const UMatchRulesDefinition* MatchRulesDef = ExperienceSubsystem->GetRules();
		const ULevelStreamingDynamic* MapStreamingInstance = ExperienceSubsystem->GetMapInstance();
		if (ExperienceState != EExperienceState::ExperienceReady || !MatchRulesDef || !MapStreamingInstance || !MapStreamingInstance->GetLoadedLevel())
		{
			RejectAdmission(TEXT("ContentFailed"));
			return;
		}

		if (Participants.Num() >= MatchRulesDef->Players.MaximumParticipants)
		{
			RejectAdmission(TEXT("NoSeat"));
			return;
		}

		AParticipantState* ParticipantState = HeroController->GetPlayerState<AParticipantState>();
		if (!ParticipantState)
		{
			RejectAdmission(TEXT("ParticipantUnavailable"));
			return;
		}

		FResumeCredential IssuedCredential;
		IssuedCredential.Match = MatchState->MatchId;
		IssuedCredential.Participant = FGuid::NewGuid();
		FSeat NewSeat;
		if (!ResumeCredential::Generate(IssuedCredential.Secret) || !ResumeCredential::Digest(IssuedCredential.Secret, NewSeat.Digest))
		{
			IssuedCredential.Clear();
			RejectAdmission(TEXT("SecureRandomUnavailable"));
			return;
		}

		ParticipantState->ParticipantId = IssuedCredential.Participant;
		ParticipantState->MatchId = MatchState->MatchId;
		ParticipantState->bConnected = false;
		Participants.Add(ParticipantState);
		NewSeat.Participant = ParticipantState;
		Seats.Add(MoveTemp(NewSeat));
		SelectedSeat = &Seats.Last();
		HeroController->ClientStoreCredential(IssuedCredential);
		IssuedCredential.Clear();
	}

	SelectedSeat->ReservedBy = HeroController;
	SelectedSeat->Request = FGuid::NewGuid();
	// 内容预留最多 65 秒，但等待加载或再次预留不能越过本连接已有截止时间。
	SelectedSeat->Deadline = FMath::Min(ConnectionDeadline, FPlatformTime::Seconds() + 65);
	PendingConnections[HeroController] = SelectedSeat->Deadline;
	UE_LOG(LogMobaControl, Display, TEXT("AdmissionReserved Participant=%s Request=%s Resume=%d"), *SelectedSeat->Participant->ParticipantId.ToString(), *SelectedSeat->Request.ToString(),
	       bResume);
	// 本地拥有端可同步提交并释放失败席位；回调后不得再访问 Seat。
	HeroController->ClientAdmissionResult(SelectedSeat->Request, NAME_None);
}

void AMatchGameMode::CompleteAdmission(AHeroPlayerController* HeroController, FGuid AdmissionRequestId, FGuid ExperienceInstanceId, bool bContentLoaded)
{
	FSeat* ReservedSeat = Seats.FindByPredicate([&](const FSeat& CandidateSeat)
	{
		return CandidateSeat.ReservedBy == HeroController && CandidateSeat.Request == AdmissionRequestId && AdmissionRequestId.IsValid();
	});
	if (!ReservedSeat)
	{
		HeroController->ClientAdmissionResult(FGuid(), TEXT("StaleRequest"));
		return;
	}

	if (!bMatchAdmissionOpen || !bContentLoaded || FPlatformTime::Seconds() >= ReservedSeat->Deadline)
	{
		ReleaseAdmission(HeroController, bContentLoaded ? TEXT("CommitInvalid") : TEXT("ContentFailed"));
		return;
	}

	AMatchGameState* MatchState = GetGameState<AMatchGameState>();
	AParticipantState* ParticipantState = ReservedSeat->Participant.Get();
	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	if (!IsAdmissionContextCurrent(MatchState, ParticipantState, ExperienceInstanceId) || ParticipantState->bConnected
		|| ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		ReleaseAdmission(HeroController, TEXT("CommitInvalid"));
		return;
	}

	if (ReservedSeat->bCommitted && !IsValid(ParticipantState->PrimaryHero))
	{
		ReleaseAdmission(HeroController, TEXT("CommitInvalid"));
		return;
	}

	const UMatchRulesDefinition* MatchRulesDef = ExperienceSubsystem->GetRules();
	const int32 TeamId = ParticipantState->TeamId == INDEX_NONE
		                     ? MatchRulesDef->TeamRules.Teams[Participants.IndexOfByKey(ParticipantState) % MatchRulesDef->TeamRules.Teams.Num()]
		                     : ParticipantState->TeamId;
	const int32 TeamParticipantCount = Participants.FilterByPredicate(
		[ParticipantState, TeamId](const AParticipantState* ExistingParticipantState)
		{
			return ExistingParticipantState && ExistingParticipantState != ParticipantState && ExistingParticipantState->TeamId == TeamId;
		}).Num();

	if (TeamParticipantCount >= MatchRulesDef->Players.MaximumPlayersPerTeam)
	{
		ReleaseAdmission(HeroController, TEXT("TeamFull"));
		return;
	}

	AParticipantState* TemporaryPlayerState = HeroController->GetPlayerState<AParticipantState>();
	HeroController->SetPlayerState(ParticipantState);
	ParticipantState->SetOwner(HeroController);
	ParticipantState->SetLifeSpan(0);
	ParticipantState->bConnected = true;
	if (ParticipantState->TeamId == INDEX_NONE)
	{
		ParticipantState->TeamId = TeamId;
	}

	++ParticipantState->ConnectionEpoch;
	HeroController->AcceptContentReady();
	if (TemporaryPlayerState && TemporaryPlayerState != ParticipantState)
	{
		MatchState->RemovePlayerState(TemporaryPlayerState);
		TemporaryPlayerState->Destroy();
	}

	if (AHeroCharacter* HeroCharacter = ParticipantState->PrimaryHero)
	{
		AController* PreviousController = HeroCharacter->GetController();
		if (PreviousController)
		{
			PreviousController->UnPossess();
			PreviousController->Destroy();
		}

		HeroCharacter->ConnectionEpoch = ParticipantState->ConnectionEpoch;
		HeroCharacter->bInputReady = false;
		HeroController->Possess(HeroCharacter);
		HeroCharacter->RefreshActorInfo();
		HeroCharacter->ForceNetUpdate();
		HeroController->ClientBindingState(HeroCharacter->EntityId, HeroCharacter->ConnectionEpoch, HeroCharacter->GetLifeSnapshot().Generation, HeroCharacter->GetLifeSnapshot().DeathCount);
		UE_LOG(LogMobaControl, Display, TEXT("ResumeCommitted Participant=%s Entity=%s Epoch=%u Hero=%s ASC=%s Sources=%d Init=%u Revision=%u Health=%.1f Primary=%.1f Life=%d Deaths=%u"),
		       *ParticipantState->ParticipantId.ToString(), *HeroCharacter->EntityId.ToString(), HeroCharacter->ConnectionEpoch, *HeroCharacter->GetName(),
		       *HeroCharacter->GetHeroAbilitySystem()->GetName(),
		       HeroCharacter->GetHeroAbilitySystem()->GetSourceCount(), HeroCharacter->InitializationGeneration, HeroCharacter->RequiredLoadoutRevision,
		       HeroCharacter->GetAttributes()->GetHealth(),
		       HeroCharacter->GetAttributes()->GetMana(), int32(HeroCharacter->GetLifeSnapshot().State), HeroCharacter->GetLifeSnapshot().DeathCount);
	}

	TryPrepareHero(HeroController);
	if (!ParticipantState->PrimaryHero)
	{
		ParticipantState->bConnected = false;
		ReleaseAdmission(HeroController, TEXT("HeroUnavailable"));
		return;
	}

	ReservedSeat->bCommitted = true;
	ReservedSeat->ReservedBy.Reset();
	ReservedSeat->Request.Invalidate();
	PendingConnections.Remove(HeroController);
	ParticipantState->ForceNetUpdate();
	HeroController->ForceNetUpdate();
	HeroController->ClientAdmissionResult(AdmissionRequestId, TEXT("Bound"));
}

void AMatchGameMode::ReleaseAdmission(AHeroPlayerController* HeroController, FName AdmissionResultReason)
{
	if (ContentPendingConnections.Remove(HeroController) > 0)
	{
		UE_LOG(LogMobaControl, Display, TEXT("AdmissionWaitingReleased Reason=%s"), *AdmissionResultReason.ToString());
		HeroController->ClientAdmissionResult(FGuid(), AdmissionResultReason);
	}

	for (int32 SeatIndex = Seats.Num() - 1; SeatIndex >= 0; --SeatIndex)
	{
		FSeat& ReservedSeat = Seats[SeatIndex];
		if (ReservedSeat.ReservedBy != HeroController)
		{
			continue;
		}

		ReservedSeat.ReservedBy.Reset();
		ReservedSeat.Request.Invalidate();
		if (!ReservedSeat.bCommitted)
		{
			if (AParticipantState* ParticipantState = ReservedSeat.Participant.Get())
			{
				Participants.Remove(ParticipantState);
				if (HeroController->PlayerState == ParticipantState)
				{
					HeroController->SetPlayerState(nullptr);
				}

				ParticipantState->Destroy();
			}

			Seats.RemoveAt(SeatIndex);
		}

		UE_LOG(LogMobaControl, Display, TEXT("AdmissionReleased Reason=%s"), *AdmissionResultReason.ToString());
		HeroController->ClientAdmissionResult(FGuid(), AdmissionResultReason);
	}

	// 失败连接仍受原始握手截止时间约束；不能清掉timeout后永久留下临时PlayerState。
	if (AdmissionResultReason == TEXT("Disconnected"))
	{
		PendingConnections.Remove(HeroController);
	}
}

void AMatchGameMode::FinishMatch()
{
	if (!HasAuthority() || !bMatchAdmissionOpen)
	{
		return;
	}

	bMatchAdmissionOpen = false;
	if (auto* MatchState = GetGameState<AMatchGameState>())
	{
		MatchState->bMatchEnded = true;
		MatchState->ForceNetUpdate();
	}

	for (const auto& PendingConnection : PendingConnections)
	{
		if (AHeroPlayerController* CurrentController = PendingConnection.Key.Get())
		{
			CurrentController->ClientAdmissionResult(FGuid(), TEXT("MatchEnded"));
			if (auto* TemporaryPlayerState = CurrentController->GetPlayerState<AParticipantState>(); TemporaryPlayerState && !Participants.Contains(TemporaryPlayerState))
			{
				CurrentController->SetPlayerState(nullptr);
				TemporaryPlayerState->Destroy();
			}

			CurrentController->SetLifeSpan(1);
		}
	}

	Seats.Reset();
	PendingConnections.Reset();
	ContentPendingConnections.Reset();
	const auto RetainedParticipantStates = Participants;
	Participants.Reset();
	for (AParticipantState* ParticipantState : RetainedParticipantStates)
	{
		if (!IsValid(ParticipantState))
		{
			continue;
		}

		if (AHeroCharacter* HeroCharacter = ParticipantState->PrimaryHero)
		{
			AController* CurrentController = HeroCharacter->GetController();
			if (CurrentController)
			{
				CurrentController->UnPossess();
				if (CurrentController->IsA<AAIController>())
				{
					CurrentController->Destroy();
				}
			}

			HeroCharacter->Destroy();
		}

		if (auto* CurrentController = Cast<AHeroPlayerController>(ParticipantState->GetOwner()))
		{
			CurrentController->ClientAdmissionResult(FGuid(), TEXT("MatchEnded"));
			CurrentController->SetPlayerState(nullptr);
		}

		ParticipantState->Destroy();
	}

	UE_LOG(LogMobaControl, Display, TEXT("MatchFinished CredentialsRevoked ParticipantsReleased=%d"), RetainedParticipantStates.Num());
	if (GetNetMode() == NM_Standalone && !GetWorld()->bIsTearingDown)
	{
		GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->NotifyMatchEnded(GetWorld());
	}
}

