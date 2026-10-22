#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Feedback/ClientFeedbackModel.h"
#include "ClientFeedbackWidget.generated.h"

class UTextBlock;

/** 当前拥有端最近的通用拒绝提示，不关联未公开的 Operation 或技能槽。 */
UCLASS(Abstract, Blueprintable)
class UClientFeedbackWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UClientFeedbackWidget(const FObjectInitializer& ObjectInitializer);


protected:
	virtual void NativeConstruct() override;

	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	virtual void NativeDestruct() override;


private:
	void RefreshText();


public:
	FText GetIntentText() const { return Feedback.GetIntentText(); }
	FText GetItemText() const { return Feedback.GetItemText(); }


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> IntentText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> ItemText;

	FClientFeedbackModel Feedback;
};
