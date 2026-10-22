#include "Frontend/FrontendGameMode.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Experience/MobaAssetManager.h"
#include "Session/GameFlowSubsystem.h"
#include "Session/MobaSessionSubsystem.h"

AFrontendPlayerController::AFrontendPlayerController()
{
	bShowMouseCursor = true;
}

void AFrontendPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		SetInputMode(FInputModeUIOnly());
	}
}

AFrontendGameMode::AFrontendGameMode()
{
	DefaultPawnClass = nullptr;
	PlayerControllerClass = AFrontendPlayerController::StaticClass();
}

void AFrontendGameMode::StartPlay()
{
	UGameFlowSubsystem* GameFlowSubsystem = GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
	GameFlowSubsystem->EnterFrontend(GetWorld());
	const auto* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
	FMatchLaunchSelection LaunchSelection;
	const EInitialStartupDecision StartupDecision = GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->TryResolveInitialStartup(GetWorld(),
		AssetManager ? AssetManager->GetGameConfiguration() : nullptr, LaunchSelection);
	if (StartupDecision == EInitialStartupDecision::Match)
	{
		GameFlowSubsystem->RequestLocalMatch(LaunchSelection.ModeId);
	}
	else if (StartupDecision == EInitialStartupDecision::Failure)
	{
		GameFlowSubsystem->NotifyStartupFailure();
	}

	Super::StartPlay();
}

