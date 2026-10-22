#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UObject/PrimaryAssetId.h"
#include "FrontendWidget.generated.h"

class UButton;
class UTextBlock;
class UVerticalBox;
class UFrontendModeWidget;

/** 只显示轻量目录并提交意图；World travel、Feature 与准入由核心流程负责。 */
UCLASS(Abstract, Blueprintable)
class UFrontendWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFrontendWidget(const FObjectInitializer& ObjectInitializer);


protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeDestruct() override;


public:
	void SelectMode(FPrimaryAssetId ModeId);


private:
	// 菜单意图
	UFUNCTION()
	void StartLocalMatch();

	UFUNCTION()
	void ReturnToFrontend();

	UFUNCTION()
	void SelectPreviousMode();

	UFUNCTION()
	void SelectNextMode();

	void CycleMode(int32 Direction);

	void RefreshMenu();
	void RefreshModes();


	bool IsFrontend() const;
	bool CanStart() const;
	FText GetSelectionText() const;
	FText GetStatusText() const;


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> FrontendPanel;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UVerticalBox> ModeList;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> SelectionText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> StartButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> ReturnButton;

	UPROPERTY(EditDefaultsOnly, Category="Presentation")
	TSubclassOf<UFrontendModeWidget> ModeEntryClass;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> PreviousModeButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> NextModeButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> StartFocusFrame;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> PreviousFocusFrame;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> NextFocusFrame;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> ReturnFocusFrame;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UFrontendModeWidget>> ModeEntries;

	// 按显示顺序缓存玩法资产 ID，用于判断是否需要重建条目控件。
	TArray<FPrimaryAssetId> DisplayedModes;

	FPrimaryAssetId SelectedModeId;
	bool bReportedMissingEntryClass = false;
	bool bFocusStartPending = false;
};

