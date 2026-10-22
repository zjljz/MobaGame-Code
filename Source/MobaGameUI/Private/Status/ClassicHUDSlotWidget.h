#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ClassicHUDSlotWidget.generated.h"

class UButton;
class UProgressBar;
class UTextBlock;

/** 只读技能/物品格；仅物品主动操作转交既有 Controller 入口。 */
UCLASS(Abstract, Blueprintable)
class UClassicHUDSlotWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;

	virtual void NativeDestruct() override;


public:
	void Present(const FText& Name, const FText& Detail, const FText& Key, bool bEnabled, FGuid ItemInstance = {});

	void PresentCooldown(bool bKnown, float Remaining, float Duration);


private:
	UFUNCTION()
	void UseItem();


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> ActionButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> NameText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> DetailText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> KeyText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> CooldownText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> CooldownMask;

	FGuid PresentedItem;
};
