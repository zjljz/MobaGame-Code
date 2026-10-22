#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Status/StatusPanelSnapshot.h"
#include "StatusPanelWidget.generated.h"

class UHorizontalBox;
class UProgressBar;
class UStatusTextWidget;
class UTextBlock;
class UVerticalBox;

#if WITH_DEV_AUTOMATION_TESTS
struct FStatusPanelVisualMetrics
{
	FVector2D HostSize = FVector2D::ZeroVector;
	FVector2D PanelOffset = FVector2D::ZeroVector;
	FVector2D PanelSize = FVector2D::ZeroVector;
	float HostScale = 0;
	float ReadabilityScale = 0;
	float BodyLineHeight = 0;
	bool bItemTextFits = false;
};
#endif

UCLASS(Abstract, Blueprintable)
class UStatusPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UStatusPanelWidget(const FObjectInitializer& ObjectInitializer);


protected:
	virtual void NativeConstruct() override;

	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	virtual void NativeDestruct() override;


public:
	// 只读显示
	void RefreshStatus();

	/** 仅接受值快照；每次完整替换显示，未就绪区块不保留上次内容。 */
	void PresentStatus(const FStatusPanelSnapshot& Snapshot);


private:
	// 动态条目
	bool EnsureEntries(int32 AbilityRowCount);

	void ClearEntries();


public:
	const FStatusPanelSnapshot& GetPresentedStatus() const { return PresentedStatus; }

#if WITH_DEV_AUTOMATION_TESTS
	/** 只读测试几何；Designer 决定布局和字号，绝对尺寸取自实际绘制空间。 */
	FStatusPanelVisualMetrics GetVisualMetrics() const;
#endif


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> StatusCard;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> LifeText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> HealthText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> ManaText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> GoldText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> HealthBar;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> ManaBar;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UVerticalBox> AbilityBox;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UHorizontalBox> ItemBox;

	UPROPERTY(EditDefaultsOnly, Category="Status")
	TSubclassOf<UStatusTextWidget> AbilityRowClass;

	UPROPERTY(EditDefaultsOnly, Category="Status")
	TSubclassOf<UStatusTextWidget> ItemSlotClass;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStatusTextWidget>> AbilityRows;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStatusTextWidget>> ItemSlots;

	FStatusPanelSnapshot PresentedStatus;
	bool bPresentationReady = false;
};
