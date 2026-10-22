#include "TargetPreview/PreviewSlotWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "TargetPreview/TargetPreviewWidget.h"

void UPreviewSlotWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(false);
	SelectButton->OnClicked.AddUniqueDynamic(this, &ThisClass::ChooseSlot);
}

void UPreviewSlotWidget::NativeDestruct()
{
	OwnerPreview.Reset();
	PreviewSlot = {};
	Super::NativeDestruct();
}

void UPreviewSlotWidget::PresentSlot(UTargetPreviewWidget* PreviewWidget, FGameplayTag InSlot, const FText& Label, bool bEnabled, bool bSelected)
{
	OwnerPreview = PreviewWidget;
	PreviewSlot = InSlot;
	LabelText->SetText(Label);
	SelectButton->SetIsEnabled(bEnabled);
	SelectionMarker->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
}

void UPreviewSlotWidget::ChooseSlot()
{
	if (UTargetPreviewWidget* PreviewWidget = OwnerPreview.Get())
	{
		PreviewWidget->ChooseSlot(PreviewSlot);
	}
}

