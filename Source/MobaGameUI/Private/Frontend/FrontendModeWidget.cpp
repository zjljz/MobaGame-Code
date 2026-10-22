#include "Frontend/FrontendModeWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Frontend/FrontendWidget.h"

void UFrontendModeWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(false);
	SelectButton->OnClicked.AddUniqueDynamic(this, &ThisClass::ChooseMode);
}

void UFrontendModeWidget::NativeDestruct()
{
	OwnerMenu.Reset();
	ModeId = {};
	Super::NativeDestruct();
}

void UFrontendModeWidget::PresentMode(UFrontendWidget* MenuWidget, FPrimaryAssetId InModeId, const FText& Label, bool bSelected)
{
	OwnerMenu = MenuWidget;
	ModeId = InModeId;
	LabelText->SetText(Label);
	SelectionMarker->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
}

void UFrontendModeWidget::ChooseMode()
{
	if (UFrontendWidget* MenuWidget = OwnerMenu.Get())
	{
		MenuWidget->SelectMode(ModeId);
	}
}

