#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StatusTextWidget.generated.h"

class UTextBlock;

/** 技能行与只读物品格共用的数据入口；子类 Designer 分别定义布局和样式。 */
UCLASS(Abstract, Blueprintable)
class UStatusTextWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UStatusTextWidget(const FObjectInitializer& ObjectInitializer);


protected:
	virtual void NativeConstruct() override;


public:
	void SetContent(const FText& Content);


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> ContentText;

	FText PresentedContent;
};
