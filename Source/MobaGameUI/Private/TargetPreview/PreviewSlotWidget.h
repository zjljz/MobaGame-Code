#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameplayTagContainer.h"
#include "PreviewSlotWidget.generated.h"

class UButton;
class UTextBlock;
class UTargetPreviewWidget;

UCLASS(Abstract, Blueprintable)
class UPreviewSlotWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;


public:
	void PresentSlot(UTargetPreviewWidget* PreviewWidget, FGameplayTag InSlot, const FText& Label, bool bEnabled, bool bSelected);


private:
	UFUNCTION()
	void ChooseSlot();


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> SelectButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> LabelText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> SelectionMarker;

	TWeakObjectPtr<UTargetPreviewWidget> OwnerPreview;
	FGameplayTag PreviewSlot;
};

