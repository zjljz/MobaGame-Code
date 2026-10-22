#include "Session/GameFlowSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Kismet/GameplayStatics.h"
#include "Match/MatchGameMode.h"
#include "Misc/PackageName.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "Player/HeroPlayerController.h"
#include "Session/MobaSessionSubsystem.h"

#define LOCTEXT_NAMESPACE "MobaGameFlow"

namespace
{
	const UGameConfiguration* GetGameConfig()
	{
		const auto* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
		return AssetManager ? AssetManager->GetGameConfiguration() : nullptr;
	}

	bool IsWorldAsset(const UWorld* World, const TSoftObjectPtr<UWorld>& WorldAsset)
	{
		return World && !WorldAsset.IsNull() && UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) == WorldAsset.GetLongPackageName();
	}

	bool CanTravelTo(const TSoftObjectPtr<UWorld>& WorldAsset)
	{
		return !WorldAsset.IsNull() && FPackageName::DoesPackageExist(WorldAsset.GetLongPackageName());
	}
}

void UGameFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UMobaSessionSubsystem>();
	GEngine->OnTravelFailure().AddUObject(this, &ThisClass::HandleTravelFailure);
	GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::HandleNetworkFailure);
}

void UGameFlowSubsystem::Deinitialize()
{
	++GameFlowSnapshot.Generation;
	if (UWorld* World = FlowWorld.Get())
	{
		World->GetTimerManager().ClearTimer(AdvanceTimer);
	}

	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, this);
	GEngine->OnTravelFailure().RemoveAll(this);
	GEngine->OnNetworkFailure().RemoveAll(this);
	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	ObservedContent.Reset();
	FlowWorld.Reset();
	Super::Deinitialize();
}

void UGameFlowSubsystem::EnterFrontend(UWorld* World)
{
	if (!World || World->GetGameInstance() != GetGameInstance() || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	ObserveContent(World);
	SetState(GameFlowSnapshot.Failure.IsEmpty() ? EGameFlowState::Frontend : EGameFlowState::Failed);
	World->GetSubsystem<UExperienceSubsystem>()->StartFrontend();
}

bool UGameFlowSubsystem::ClaimLocalMatch(UWorld* World, FMatchLaunchSelection& OutLaunchSelection)
{
	const UGameConfiguration* GameConfig = GetGameConfig();
	if (!GameConfig || !PendingRequest.IsValid() || GameFlowSnapshot.State != EGameFlowState::StartingMatch
		|| !World || World->GetNetMode() != NM_Standalone || !IsWorldAsset(World, GameConfig->LocalMatchWorld))
	{
		return false;
	}

	OutLaunchSelection = PendingSelection;
	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	UE_LOG(LogMobaControl, Display, TEXT("LocalMatchRequestClaimed Generation=%lld Mode=%s World=%s"), GameFlowSnapshot.Generation, *OutLaunchSelection.ModeId.ToString(), *World->GetName());
	return true;
}

void UGameFlowSubsystem::ObserveMatch(UWorld* World)
{
	if (!World || World->GetGameInstance() != GetGameInstance() || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ObserveContent(World);
	GameFlowSnapshot.Failure = FText::GetEmpty();
	SetState(EGameFlowState::StartingMatch);
	QueueAdvance();
}

void UGameFlowSubsystem::NotifyMatchReady(UWorld* World)
{
	if (World == FlowWorld.Get() && GameFlowSnapshot.State == EGameFlowState::StartingMatch && !IsFrontendWorld(World))
	{
		SetState(EGameFlowState::InMatch);
	}
}

void UGameFlowSubsystem::NotifyMatchEnded(UWorld* World)
{
	if (World == FlowWorld.Get() && !World->bIsTearingDown && GameFlowSnapshot.State != EGameFlowState::ReturningFrontend)
	{
		RequestFrontend();
	}
}

void UGameFlowSubsystem::NotifyStartupFailure()
{
	Fail(LOCTEXT("StartupFailure", "启动选择无效；请检查 moba.DevModeIndex 和启用模式配置。"));
}

void UGameFlowSubsystem::NotifyAdmissionFailure(UWorld* World, FName Reason)
{
	if (World != FlowWorld.Get() || GameFlowSnapshot.State != EGameFlowState::StartingMatch)
	{
		return;
	}

	GameFlowSnapshot.Failure = FText::Format(LOCTEXT("AdmissionFailure", "比赛准入失败：{0}。请重试或选择恢复原比赛。"), FText::FromName(Reason));
	RequestFrontend();
}

void UGameFlowSubsystem::BeginRemoteConnection()
{
	++GameFlowSnapshot.Generation;
	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	GameFlowSnapshot.Failure = FText::GetEmpty();
	if (UWorld* World = GetWorld())
	{
		ObserveContent(World);
	}

	SetState(EGameFlowState::StartingMatch);
}

bool UGameFlowSubsystem::RequestLocalMatch(FPrimaryAssetId ModeId)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	UWorld* World = GetWorld();
	const UGameConfiguration* GameConfig = GetGameConfig();
	if (!World || World->bIsTearingDown || World->GetNetMode() != NM_Standalone || !IsFrontendWorld(World)
		|| (GameFlowSnapshot.State != EGameFlowState::Frontend && GameFlowSnapshot.State != EGameFlowState::Failed))
	{
		return false;
	}

	if (!GameConfig || !GameConfig->FindEnabledMode(ModeId) || !CanTravelTo(GameConfig->LocalMatchWorld))
	{
		Fail(LOCTEXT("InvalidLocalMatch", "本地比赛不可用：模式未启用或比赛宿主地图缺失。"));
		return false;
	}

	++GameFlowSnapshot.Generation;
	GameFlowSnapshot.Failure = FText::GetEmpty();
	PendingRequest = FGuid::NewGuid();
	PendingSelection.ModeId = ModeId;
	PendingSelection.ModeConfigName = NAME_None;
	GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequestFreshJoin();
	ObserveContent(World);
	SetState(EGameFlowState::StartingMatch);
	World->GetSubsystem<UExperienceSubsystem>()->StopExperience();
	QueueAdvance();
	return true;
#endif
}

bool UGameFlowSubsystem::RequestFrontend()
{
	UWorld* World = GetWorld();
	const UGameConfiguration* GameConfig = GetGameConfig();
	if (!World || World->bIsTearingDown || World->IsBeingCleanedUp() || World->GetNetMode() == NM_DedicatedServer
		|| GameFlowSnapshot.State == EGameFlowState::ReturningFrontend)
	{
		return false;
	}

	if (World->GetNetMode() == NM_ListenServer)
	{
		Fail(LOCTEXT("ListenHostReturn", "Listen Server 宿主离场策略尚未提供；不会替所有连接结束比赛。"));
		return false;
	}

	if (!GameConfig || !CanTravelTo(GameConfig->FrontendWorld))
	{
		Fail(LOCTEXT("MissingFrontend", "无法返回：Frontend 地图未配置或不存在。"));
		return false;
	}

	++GameFlowSnapshot.Generation;
	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	ObserveContent(World);
	SetState(EGameFlowState::ReturningFrontend);
	QueueAdvance();
	return true;
}

void UGameFlowSubsystem::ObserveContent(UWorld* World)
{
	if (UWorld* PreviousWorld = FlowWorld.Get())
	{
		PreviousWorld->GetTimerManager().ClearTimer(AdvanceTimer);
	}

	AdvanceTimer.Invalidate();
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, this);
	FlowWorld = World;
	ObservedContent = World->GetSubsystem<UExperienceSubsystem>();
	GMP::FMessageUtils::ListenObjectMessage(ObservedContent.Get(), MOBA_MESSAGE_EXPERIENCE_CHANGED, this,
		[this](const FExperienceSnapshot&)
		{
			QueueAdvance();
		});
}

void UGameFlowSubsystem::QueueAdvance()
{
	UWorld* World = FlowWorld.Get();
	if (!World || World->bIsTearingDown || World->GetTimerManager().TimerExists(AdvanceTimer))
	{
		return;
	}

	const int64 Generation = GameFlowSnapshot.Generation;
	const TWeakObjectPtr<UWorld> ExpectedWorld(World);
	AdvanceTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, Generation, ExpectedWorld]
		{
			AdvanceTimer.Invalidate();
			if (Generation == GameFlowSnapshot.Generation && ExpectedWorld.IsValid() && ExpectedWorld == FlowWorld && !ExpectedWorld->bIsTearingDown)
			{
				Advance();
			}
		}));
}

void UGameFlowSubsystem::Advance()
{
	UExperienceSubsystem* ExperienceSubsystem = ObservedContent.Get();
	const UGameConfiguration* GameConfig = GetGameConfig();
	if (!ExperienceSubsystem || !GameConfig)
	{
		return;
	}

	const EExperienceState ExperienceState = ExperienceSubsystem->GetSnapshot().State;
	const bool bExperienceStopped = (ExperienceState == EExperienceState::Unloaded || ExperienceState == EExperienceState::Failed);
	if (GameFlowSnapshot.State == EGameFlowState::ReturningFrontend)
	{
		UWorld* World = FlowWorld.Get();

		// 离开准入/结束通知栈后才清理；NM_Client 不调用服务器 FinishMatch。
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (auto* HeroController = Cast<AHeroPlayerController>(It->Get()); HeroController && HeroController->IsLocalController())
			{
				HeroController->PrepareForFrontend();
			}
		}

		if (World->GetNetMode() == NM_Standalone)
		{
			if (auto* MatchGameMode = World->GetAuthGameMode<AMatchGameMode>())
			{
				MatchGameMode->FinishMatch();
			}
		}

		if (!bExperienceStopped)
		{
			ExperienceSubsystem->StopExperience();
			return;
		}

		if (IsFrontendWorld(FlowWorld.Get()))
		{
			EnterFrontend(FlowWorld.Get());
		}
		else
		{
			TravelTo(GameConfig->FrontendWorld);
		}
	}
	else if (GameFlowSnapshot.State == EGameFlowState::StartingMatch && PendingRequest.IsValid() && bExperienceStopped)
	{
		TravelTo(GameConfig->LocalMatchWorld);
	}
	else if (ExperienceState == EExperienceState::Failed && GameFlowSnapshot.State != EGameFlowState::Failed)
	{
		GameFlowSnapshot.Failure = FText::Format(LOCTEXT("ContentFailed", "内容加载失败：{0}。可返回前台后重试。"), FText::FromString(UEnum::GetValueAsString(ExperienceSubsystem->GetSnapshot().Failure)));
		if (IsFrontendWorld(FlowWorld.Get()))
		{
			SetState(EGameFlowState::Failed);
		}
		else
		{
			RequestFrontend();
		}
	}
}

void UGameFlowSubsystem::TravelTo(const TSoftObjectPtr<UWorld>& Destination)
{
	if (!CanTravelTo(Destination))
	{
		Fail(LOCTEXT("TravelTargetMissing", "切换失败：目标地图不存在。"));
		return;
	}

	UE_LOG(LogMobaControl, Display, TEXT("GameFlowTravel Generation=%lld State=%s Map=%s"), GameFlowSnapshot.Generation, *UEnum::GetValueAsString(GameFlowSnapshot.State), *Destination.GetLongPackageName());

	// 浏览请求已提交后，不再让旧 World 的内容通知重复提交 travel。
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, this);
	ObservedContent.Reset();
	UGameplayStatics::OpenLevel(FlowWorld.Get(), FName(*Destination.GetLongPackageName()));
}

void UGameFlowSubsystem::SetState(EGameFlowState State)
{
	GameFlowSnapshot.State = State;
	UE_LOG(LogMobaControl, Display, TEXT("GameFlow Generation=%lld State=%s"), GameFlowSnapshot.Generation, *UEnum::GetValueAsString(State));
	const FGameFlowSnapshot CommittedFlowSnapshot = GameFlowSnapshot;
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_GAME_FLOW_CHANGED, CommittedFlowSnapshot);
}

void UGameFlowSubsystem::Fail(const FText& Reason)
{
	++GameFlowSnapshot.Generation;
	PendingRequest.Invalidate();
	PendingSelection = FMatchLaunchSelection();
	GameFlowSnapshot.Failure = Reason;
	SetState(EGameFlowState::Failed);
}

void UGameFlowSubsystem::HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& Error)
{
	if (!World || World->GetGameInstance() != GetGameInstance() || GameFlowSnapshot.State == EGameFlowState::Frontend || GameFlowSnapshot.State == EGameFlowState::Failed)
	{
		return;
	}

	// 不将引擎错误中的连接串转发给 UI 或日志。
	Fail(FText::Format(LOCTEXT("TravelFailure", "地图切换失败（{0}），请返回前台重试。"), FText::AsNumber(int32(FailureType))));
	if (IsFrontendWorld(World))
	{
		ObserveContent(World);
		World->GetSubsystem<UExperienceSubsystem>()->StartFrontend();
	}
}

void UGameFlowSubsystem::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& Error)
{
	if (!World || World->GetGameInstance() != GetGameInstance() || (World->GetNetMode() != NM_Client && GameFlowSnapshot.State != EGameFlowState::StartingMatch)
		|| GameFlowSnapshot.State == EGameFlowState::ReturningFrontend)
	{
		return;
	}

	GameFlowSnapshot.Failure = FText::Format(LOCTEXT("NetworkFailure", "连接已中断（{0}）；原比赛恢复凭据仍保留。"), FText::AsNumber(int32(FailureType)));
	RequestFrontend();
}

bool UGameFlowSubsystem::IsFrontendWorld(const UWorld* World) const
{
	const UGameConfiguration* GameConfig = GetGameConfig();
	return GameConfig && IsWorldAsset(World, GameConfig->FrontendWorld);
}

bool UGameFlowSubsystem::CanStartLocalMatch() const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	const UWorld* World = GetWorld();
	const UExperienceSubsystem* ExperienceSubsystem = World ? World->GetSubsystem<UExperienceSubsystem>() : nullptr;
	return World && World->GetNetMode() == NM_Standalone && IsFrontendWorld(World) && ExperienceSubsystem
		&& ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady
		&& (GameFlowSnapshot.State == EGameFlowState::Frontend || GameFlowSnapshot.State == EGameFlowState::Failed);
#endif
}

#undef LOCTEXT_NAMESPACE

