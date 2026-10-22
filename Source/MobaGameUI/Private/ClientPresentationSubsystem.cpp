#include "ClientPresentationSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "ClientUIConfiguration.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "Frontend/FrontendWidget.h"
#include "GameFramework/PlayerController.h"
#include "Match/MatchGameState.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "Session/GameFlowSubsystem.h"
#include "Status/StatusPanelWidget.h"
#include "Status/ClassicHUDWidget.h"
#include "TargetPreview/TargetPreviewWidget.h"
#include "Widgets/SViewport.h"

UClientPresentationSubsystem::UClientPresentationSubsystem() : FTickableGameObject(ETickableTickType::Never)
{
}

bool UClientPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer() && !IsRunningCommandlet();
}

void UClientPresentationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bInitialized = true;
	Configuration = ConfigurationAsset.LoadSynchronous();
	if (!Configuration)
	{
		UE_LOG(LogMobaControl, Error, TEXT("UIConfigurationMissing Asset=%s"), *ConfigurationAsset.ToString());
	}
	else
	{
		const UClass* Classes[] = {Configuration->FrontendClass.Get(), Configuration->StatusClass.Get(), Configuration->TargetPreviewClass.Get(), Configuration->FeedbackClass.Get()};
		const TCHAR* Properties[] = {TEXT("FrontendClass"), TEXT("StatusClass"), TEXT("TargetPreviewClass"), TEXT("FeedbackClass")};
		bool bValid = true;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Classes); ++Index)
		{
			if (!Classes[Index] || !Classes[Index]->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
				|| Classes[Index]->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				UE_LOG(LogMobaControl, Error, TEXT("UIConfigurationInvalid Asset=%s Property=%s Reason=ExpectedConcreteWidgetBlueprint"), *Configuration->GetPathName(), Properties[Index]);
				bValid = false;
			}
		}

		if (bValid)
		{
			RegisterLayer(TEXT("Frontend"), Configuration->FrontendClass, 100, true, false);
		}
		else
		{
			Configuration = nullptr;
		}
	}

	if (auto* GameFlowSubsystem = GetLocalPlayer()->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>())
	{
		GMP::FMessageUtils::ListenObjectMessage(GameFlowSubsystem, MOBA_MESSAGE_GAME_FLOW_CHANGED, this,
			[this](const FGameFlowSnapshot& FlowSnapshot)
			{
				if (FlowSnapshot.State == EGameFlowState::InMatch)
				{
					ApplyMatchInputMode();
				}

				RefreshLayers();
			});
	}

	// UE World 销毁接缝：只清理本地持有实例，不向项目转发自定义委托。
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &ThisClass::OnWorldBeginTearDown);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::OnWorldCleanup);
	SetTickableTickType(ETickableTickType::Conditional);
}

void UClientPresentationSubsystem::Deinitialize()
{
	bInitialized = false;
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_GAME_FLOW_CHANGED, this);
	SetTickableTickType(ETickableTickType::Never);
	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	WorldTearDownHandle.Reset();
	WorldCleanupHandle.Reset();
	RemoveLayerWidgets();
	Layers.Reset();
	Configuration = nullptr;
	RetiredWorld.Reset();
	Super::Deinitialize();
}

void UClientPresentationSubsystem::BeginDestroy()
{
	SetTickableTickType(ETickableTickType::Never);
	Super::BeginDestroy();
}

void UClientPresentationSubsystem::PlayerControllerChanged(APlayerController* NewPlayerController)
{
	Super::PlayerControllerChanged(NewPlayerController);
	RemoveLayerWidgets();
	// LocalPlayer 原生通知期间指针可能尚在切换；下一 Tick 再读取最终 Controller。
}

void UClientPresentationSubsystem::Tick(float DeltaTime)
{
	RefreshLayers();
}

bool UClientPresentationSubsystem::IsTickable() const
{
	return bInitialized && !IsTemplate();
}

UWorld* UClientPresentationSubsystem::GetTickableGameObjectWorld() const
{
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	return LocalPlayer ? LocalPlayer->GetWorld() : nullptr;
}

TStatId UClientPresentationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UClientPresentationSubsystem, STATGROUP_Tickables);
}

bool UClientPresentationSubsystem::RegisterLayer(FName LayerName, TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder, bool bAllowPointerHit, bool bMatchOnly)
{
	check(IsInGameThread());
	if (!bInitialized || bChangingLayers || LayerName.IsNone() || !WidgetClass
		|| WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return false;
	}

	if (const FClientPresentationLayer* Existing = Layers.Find(LayerName))
	{
		return Existing->WidgetClass == WidgetClass && Existing->ZOrder == ZOrder && Existing->bAllowPointerHit == bAllowPointerHit && Existing->bMatchOnly == bMatchOnly;
	}

	FClientPresentationLayer& Layer = Layers.Add(LayerName);
	Layer.WidgetClass = WidgetClass;
	Layer.ZOrder = ZOrder;
	Layer.bAllowPointerHit = bAllowPointerHit;
	Layer.bMatchOnly = bMatchOnly;
	return true;
}

bool UClientPresentationSubsystem::UnregisterLayer(FName LayerName)
{
	check(IsInGameThread());
	if (bChangingLayers)
	{
		return false;
	}

	TGuardValue<bool> Guard(bChangingLayers, true);
	if (FClientPresentationLayer* Layer = Layers.Find(LayerName); Layer && Layer->Widget)
	{
		Layer->Widget->RemoveFromParent();
		Layer->Widget = nullptr;
	}

	Layers.Remove(LayerName);
	return true;
}

void UClientPresentationSubsystem::ApplyMatchInputMode()
{
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	UWorld* World = LocalPlayer ? LocalPlayer->GetWorld() : nullptr;
	APlayerController* Controller = World ? LocalPlayer->GetPlayerController(World) : nullptr;
	if (!World || World->bIsTearingDown || !IsValid(Controller) || !Controller->IsLocalController()
		|| !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->GetGameViewportWidget().IsValid())
	{
		return;
	}

	// GameViewport 跨 OpenLevel 保留，不能依靠新 Controller 自动撤销前台 UIOnly 的 IgnoreInput。
	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(LocalPlayer->ViewportClient->GetGameViewportWidget());
	InputMode.SetHideCursorDuringCapture(false);
	Controller->SetInputMode(InputMode);
}

void UClientPresentationSubsystem::RefreshLayers()
{
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	UWorld* World = LocalPlayer ? LocalPlayer->GetWorld() : nullptr;
	APlayerController* Controller = LocalPlayer && World ? LocalPlayer->GetPlayerController(World) : nullptr;
	if (!World || !World->IsGameWorld() || World->bIsTearingDown || RetiredWorld.Get() == World
		|| World->GetNetMode() == NM_DedicatedServer || !IsValid(Controller) || Controller->IsActorBeingDestroyed()
		|| Controller->GetWorld() != World || !Controller->IsLocalController() || Controller->GetLocalPlayer() != LocalPlayer
		|| !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->Viewport)
	{
		RemoveLayerWidgets();
		return;
	}

	if (MountedController.Get() != Controller || MountedWorld.Get() != World)
	{
		RemoveLayerWidgets();
		MountedController = Controller;
		MountedWorld = World;
		if (const auto* GameFlowSubsystem = LocalPlayer->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
			GameFlowSubsystem && GameFlowSubsystem->GetSnapshot().State == EGameFlowState::InMatch)
		{
			ApplyMatchInputMode();
		}
	}

	const auto* GameFlowSubsystem = LocalPlayer->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
	const auto* MatchState = World->GetGameState<AMatchGameState>();
	const auto* ExperienceSubsystem = World->GetSubsystem<UExperienceSubsystem>();
	const bool bShowMatch = (MatchState && MatchState->MatchId.IsValid() && !MatchState->bMatchEnded && ExperienceSubsystem
		&& ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady
		&& (!GameFlowSubsystem || GameFlowSubsystem->GetSnapshot().State != EGameFlowState::ReturningFrontend));
	TGuardValue<bool> Guard(bChangingLayers, true);
	for (auto& Pair : Layers)
	{
		FClientPresentationLayer& Layer = Pair.Value;
		const bool bClassicHUD = (Pair.Key == TEXT("Status") && Configuration && Configuration->ClassicModeId.IsValid()
			&& ExperienceSubsystem && ExperienceSubsystem->GetSnapshot().ModeId == Configuration->ClassicModeId && Configuration->ClassicHUDClass);
		const TSubclassOf<UUserWidget> DesiredClass = bClassicHUD ? TSubclassOf<UUserWidget>(Configuration->ClassicHUDClass.Get()) : Layer.WidgetClass;
		if (Layer.Widget && Layer.Widget->GetClass() != DesiredClass)
		{
			Layer.Widget->RemoveFromParent();
			Layer.Widget = nullptr;
		}

		if (Layer.bMatchOnly && !bShowMatch)
		{
			if (Layer.Widget)
			{
				Layer.Widget->RemoveFromParent();
				Layer.Widget = nullptr;
			}

			continue;
		}

		if (!Layer.Widget)
		{
			Layer.Widget = CreateWidget<UUserWidget>(Controller, DesiredClass);
		}

		if (Layer.Widget && !Layer.Widget->IsInViewport())
		{
			// 默认整棵 UI 穿透；显式开启的层只允许子控件命中，根仍不拦截空白区域。
			Layer.Widget->SetVisibility(Layer.bAllowPointerHit || bClassicHUD ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::HitTestInvisible);
			Layer.Widget->AddToPlayerScreen(Layer.ZOrder);
		}
	}
}

void UClientPresentationSubsystem::RemoveLayerWidgets()
{
	TGuardValue<bool> Guard(bChangingLayers, true);
	for (auto& Pair : Layers)
	{
		if (Pair.Value.Widget)
		{
			Pair.Value.Widget->RemoveFromParent();
			Pair.Value.Widget = nullptr;
		}
	}

	MountedController.Reset();
	MountedWorld.Reset();
}

void UClientPresentationSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	if (World && (MountedWorld.Get() == World || GetTickableGameObjectWorld() == World))
	{
		RetiredWorld = World;
		RemoveLayerWidgets();
	}
}

void UClientPresentationSubsystem::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	OnWorldBeginTearDown(World);
}

UUserWidget* UClientPresentationSubsystem::FindLayer(FName LayerName) const
{
	const FClientPresentationLayer* Layer = Layers.Find(LayerName);
	return Layer && Layer->Widget && Layer->Widget->IsInViewport() ? Layer->Widget.Get() : nullptr;
}

