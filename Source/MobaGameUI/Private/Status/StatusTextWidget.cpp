#include "Status/StatusTextWidget.h"

#include "Components/TextBlock.h"
#include "MobaLogChannels.h"

UStatusTextWidget::UStatusTextWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UStatusTextWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (!ContentText)
	{
		UE_LOG(LogMobaControl, Error, TEXT("StatusTextWidgetInvalid Class=%s Reason=MissingContentTextBinding"), *GetClass()->GetPathName());
	}

	SetContent(PresentedContent);
}

void UStatusTextWidget::SetContent(const FText& Content)
{
	PresentedContent = Content;
	if (ContentText)
	{
		ContentText->SetText(PresentedContent);
	}
}
