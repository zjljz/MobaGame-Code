#include "Feedback/ClientFeedbackWidget.h"

#include "Components/TextBlock.h"
#include "HAL/PlatformTime.h"
#include "MobaLogChannels.h"
#include "Player/HeroPlayerController.h"

UClientFeedbackWidget::UClientFeedbackWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UClientFeedbackWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
	Feedback.Reset();
	if (!IntentText || !ItemText)
	{
		UE_LOG(LogMobaControl, Error, TEXT("FeedbackWidgetInvalid Class=%s Reason=MissingRequiredBinding"), *GetClass()->GetPathName());
	}

	RefreshText();
}

void UClientFeedbackWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	AHeroPlayerController* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	const FClientTargetContext Context = MobaClientTarget::ReadContext(Controller);
	if (!Context.bValid)
	{
		Feedback.Reset();
		RefreshText();
		return;
	}

	const FItemResult ItemResult = Controller->GetLastItemResult();
	Feedback.Update(Context, Controller->GetIntentResultSequence(), Controller->WasLastIntentAccepted(),
		Controller->GetLastIntentReason(), ItemResult, FPlatformTime::Seconds());
	RefreshText();
}

void UClientFeedbackWidget::NativeDestruct()
{
	Feedback.Reset();
	RefreshText();
	Super::NativeDestruct();
}

void UClientFeedbackWidget::RefreshText()
{
	if (IntentText)
	{
		IntentText->SetText(Feedback.GetIntentText());
	}

	if (ItemText)
	{
		ItemText->SetText(Feedback.GetItemText());
	}
}
