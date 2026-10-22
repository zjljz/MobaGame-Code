#include "Frontend/FrontendWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "Experience/ExperienceSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Frontend/FrontendModeWidget.h"
#include "MobaLogChannels.h"
#include "Session/GameFlowSubsystem.h"

#define LOCTEXT_NAMESPACE "MobaFrontend"

namespace
{
	const UGameConfiguration* ReadGameConfig()
	{
		const auto* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
		return AssetManager ? AssetManager->GetGameConfiguration() : nullptr;
	}
}

UFrontendWidget::UFrontendWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

void UFrontendWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	StartButton->OnClicked.AddUniqueDynamic(this, &ThisClass::StartLocalMatch);
	ReturnButton->OnClicked.AddUniqueDynamic(this, &ThisClass::ReturnToFrontend);
	PreviousModeButton->OnClicked.AddUniqueDynamic(this, &ThisClass::SelectPreviousMode);
	NextModeButton->OnClicked.AddUniqueDynamic(this, &ThisClass::SelectNextMode);
}

void UFrontendWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RefreshMenu();
	bFocusStartPending = true;
}

void UFrontendWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshMenu();
	if (bFocusStartPending && IsFrontend() && CanStart() && IsInViewport())
	{
		StartButton->SetKeyboardFocus();
		bFocusStartPending = false;
	}
}

void UFrontendWidget::NativeDestruct()
{
	ModeList->ClearChildren();
	ModeEntries.Reset();
	DisplayedModes.Reset();
	SelectedModeId = {};
	bFocusStartPending = false;
	SelectionText->SetText(FText::GetEmpty());
	StatusText->SetText(FText::GetEmpty());
	Super::NativeDestruct();
}

void UFrontendWidget::SelectMode(FPrimaryAssetId ModeId)
{
	const UGameConfiguration* GameConfig = ReadGameConfig();
	if (CanStart() && GameConfig && GameConfig->FindEnabledMode(ModeId))
	{
		SelectedModeId = ModeId;
		RefreshMenu();
	}
}

void UFrontendWidget::StartLocalMatch()
{
	if (CanStart())
	{
		GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->RequestLocalMatch(SelectedModeId);
		RefreshMenu();
	}
}

void UFrontendWidget::ReturnToFrontend()
{
	if (auto* GameFlowSubsystem = GetGameInstance()->GetSubsystem<UGameFlowSubsystem>())
	{
		GameFlowSubsystem->RequestFrontend();
	}
}

void UFrontendWidget::SelectPreviousMode()
{
	CycleMode(-1);
}

void UFrontendWidget::SelectNextMode()
{
	CycleMode(1);
}

void UFrontendWidget::CycleMode(int32 Direction)
{
	const int32 SelectedIndex = DisplayedModes.IndexOfByKey(SelectedModeId);
	if (CanStart() && DisplayedModes.Num() > 1 && SelectedIndex != INDEX_NONE)
	{
		SelectMode(DisplayedModes[(SelectedIndex + Direction + DisplayedModes.Num()) % DisplayedModes.Num()]);
	}
}

void UFrontendWidget::RefreshMenu()
{
	RefreshModes();
	FrontendPanel->SetVisibility(IsFrontend() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	SelectionText->SetText(GetSelectionText());
	const FText Status = GetStatusText();
	StatusText->SetText(Status);
	StatusText->SetVisibility(Status.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	StartButton->SetIsEnabled(CanStart());
	const bool bCanCycle = (DisplayedModes.Num() > 1 && CanStart());
	PreviousModeButton->SetIsEnabled(bCanCycle);
	NextModeButton->SetIsEnabled(bCanCycle);
	const ESlateVisibility ArrowVisibility = DisplayedModes.Num() > 1 ? ESlateVisibility::Visible : ESlateVisibility::Hidden;
	PreviousModeButton->SetVisibility(ArrowVisibility);
	NextModeButton->SetVisibility(ArrowVisibility);
	StartFocusFrame->SetVisibility(StartButton->HasKeyboardFocus() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	PreviousFocusFrame->SetVisibility(PreviousModeButton->HasKeyboardFocus() && bCanCycle ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	NextFocusFrame->SetVisibility(NextModeButton->HasKeyboardFocus() && bCanCycle ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	ReturnFocusFrame->SetVisibility(ReturnButton->HasKeyboardFocus() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	const auto* GameFlowSubsystem = GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
	const bool bCanReturn = (!IsFrontend() || (GameFlowSubsystem && (GameFlowSubsystem->GetSnapshot().State == EGameFlowState::Failed
		|| GameFlowSubsystem->GetSnapshot().State == EGameFlowState::StartingMatch)));
	ReturnButton->SetVisibility(bCanReturn ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UFrontendWidget::RefreshModes()
{
	const UGameConfiguration* GameConfig = ReadGameConfig();
	if (!ModeEntryClass || ModeEntryClass->HasAnyClassFlags(CLASS_Abstract))
	{
		if (!bReportedMissingEntryClass)
		{
			UE_LOG(LogMobaControl, Error, TEXT("UIConfigurationInvalid Widget=%s Property=ModeEntryClass"), *GetClass()->GetPathName());
			bReportedMissingEntryClass = true;
		}

		return;
	}

	TArray<FPrimaryAssetId> ModeIds;
	if (GameConfig)
	{
		for (const FEnabledModeEntry& Entry : GameConfig->EnabledModes)
		{
			ModeIds.Add(Entry.ModeId);
		}

		if (!GameConfig->FindEnabledMode(SelectedModeId))
		{
			SelectedModeId = ModeIds.IsEmpty() ? FPrimaryAssetId() : ModeIds[0];
		}
	}
	else
	{
		SelectedModeId = {};
	}

	if (DisplayedModes != ModeIds)
	{
		ModeList->ClearChildren();
		ModeEntries.Reset();
		DisplayedModes = ModeIds;
		for (int32 Index = 0; Index < ModeIds.Num(); ++Index)
		{
			UFrontendModeWidget* EntryWidget = CreateWidget<UFrontendModeWidget>(GetOwningPlayer(), ModeEntryClass);
			if (!EntryWidget)
			{
				UE_LOG(LogMobaControl, Error, TEXT("UIEntryCreationFailed Widget=%s Property=ModeEntryClass"), *GetClass()->GetPathName());
				ModeList->ClearChildren();
				ModeEntries.Reset();
				return;
			}

			ModeList->AddChildToVerticalBox(EntryWidget);
			ModeEntries.Add(EntryWidget);
		}
	}

	for (int32 Index = 0; Index < ModeEntries.Num(); ++Index)
	{
		const FEnabledModeEntry* Entry = GameConfig->FindEnabledMode(DisplayedModes[Index]);
		const bool bSelected = (Entry->ModeId == SelectedModeId);
		ModeEntries[Index]->PresentMode(this, Entry->ModeId, Entry->DisplayName, bSelected);
		ModeEntries[Index]->SetVisibility(bSelected ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
}

bool UFrontendWidget::IsFrontend() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const auto* GameFlowSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	return GameFlowSubsystem && GameFlowSubsystem->IsFrontendWorld(GetWorld());
}

bool UFrontendWidget::CanStart() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const auto* GameFlowSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	const UGameConfiguration* GameConfig = ReadGameConfig();
	return GameConfig && GameConfig->FindEnabledMode(SelectedModeId) && GameFlowSubsystem && GameFlowSubsystem->CanStartLocalMatch();
}

FText UFrontendWidget::GetSelectionText() const
{
	const UGameConfiguration* GameConfig = ReadGameConfig();
	const FEnabledModeEntry* Entry = GameConfig ? GameConfig->FindEnabledMode(SelectedModeId) : nullptr;
	return Entry ? Entry->Description : LOCTEXT("NoModes", "当前没有可用玩法。");
}

FText UFrontendWidget::GetStatusText() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const auto* GameFlowSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	if (!GameFlowSubsystem)
	{
		return FText::GetEmpty();
	}

	const FGameFlowSnapshot& FlowSnapshot = GameFlowSubsystem->GetSnapshot();
	if (!FlowSnapshot.Failure.IsEmpty())
	{
		return FlowSnapshot.Failure;
	}

	if (FlowSnapshot.State == EGameFlowState::StartingMatch)
	{
		return LOCTEXT("Loading", "正在进入本地比赛：加载内容并等待准入和角色就绪……");
	}

	if (FlowSnapshot.State == EGameFlowState::ReturningFrontend)
	{
		return LOCTEXT("Returning", "正在清理本地内容并返回 Frontend……");
	}

	const UExperienceSubsystem* ExperienceSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UExperienceSubsystem>() : nullptr;
	if (IsFrontend() && GetWorld()->GetNetMode() != NM_Standalone)
	{
		return LOCTEXT("LocalTopology", "本地开发入口需要 Standalone、1 名玩家。当前联网 PIE 不创建本地比赛，请切换 Play 模式后体验。");
	}

	if (IsFrontend() && ExperienceSubsystem && ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		return LOCTEXT("PreparingFrontend", "正在准备 Frontend……");
	}

	return FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE

