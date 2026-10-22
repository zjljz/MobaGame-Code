#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TargetPreview/TargetPreviewModel.h"
#include "TargetPreviewWidget.generated.h"

class UVerticalBox;
class UTextBlock;
class UButton;
class UPreviewSlotWidget;

/** 只选择本地预览槽；几何有效不代表服务器命中。 */
UCLASS(Abstract, Blueprintable)
class UTargetPreviewWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UTargetPreviewWidget(const FObjectInitializer& ObjectInitializer);


protected:
	virtual void NativeOnInitialized() override;

	virtual void NativeConstruct() override;

	virtual void NativeDestruct() override;

	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;


public:
	void ChooseSlot(FGameplayTag PreviewSlot);


private:
	// 当前拥有端预览
	void RefreshPreview();

	void RebuildSlotButtons();

	UFUNCTION()
	void ClearSelection();

	void ClearPreview();


public:
	FGameplayTag GetSelectedSlot() const { return Selection.GetSelectedSlot(); }
	FText GetSummaryText() const { return Summary; }
	int32 GetOutlinePointCount() const { return Outline.Num(); }


private:
	FClientTargetContext Context;
	FTargetPreviewSelection Selection;
	TArray<FVector> Outline;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UVerticalBox> SlotButtons;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> SummaryText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> ClearButton;

	UPROPERTY(EditDefaultsOnly, Category="Presentation")
	TSubclassOf<UPreviewSlotWidget> SlotButtonClass;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPreviewSlotWidget>> SlotEntries;

	TArray<FGameplayTag> DisplayedSlots;
	bool bReportedMissingSlotClass = false;
	FText Summary;
};
