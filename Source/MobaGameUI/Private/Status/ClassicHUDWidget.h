#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameplayEffectTypes.h"
#include "Status/StatusPanelSnapshot.h"
#include "ClassicHUDWidget.generated.h"

class AHeroCharacter;
class AParticipantState;
class UButton;
class UClassicHUDSlotWidget;
class UHeroAbilitySystemComponent;
class UHeroAttributeComponent;
class UHorizontalBox;
class UProgressBar;
class UTextBlock;
class UUniformGridPanel;

/** Classic 的展示层：Designer 管理布局，现有拥有链和 GAS 提供全部运行数据。 */
UCLASS(Abstract, Blueprintable)
class UClassicHUDWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;

	virtual void NativeDestruct() override;


private:
	// 绑定与刷新
	void CheckBinding();

	void UnbindSources();

	void QueueRefresh();

	void RefreshStatus();

	void RefreshCooldowns();

	FText ReadInputKey(FGameplayTag AbilitySlot) const;


	// 属性浮层
	UFUNCTION()
	void ToggleAttributes();

	UFUNCTION()
	void CloseAttributes();


public:
#if WITH_DEV_AUTOMATION_TESTS
	const FStatusPanelSnapshot& GetPresentedStatus() const { return PresentedStatus; }
#endif


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> StatusCard;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> LifeText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> CompactText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> HealthText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> ManaText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> GoldText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> LevelText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> ExperienceText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> ExperienceBar;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> HealthBar;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UProgressBar> ManaBar;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UHorizontalBox> AbilityBox;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UUniformGridPanel> ItemBox;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> AttributesButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> CloseAttributesButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> AttributesPanel;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> AttributesText;

	UPROPERTY(EditDefaultsOnly, Category="HUD")
	TSubclassOf<UClassicHUDSlotWidget> SlotWidgetClass;

	UPROPERTY(EditDefaultsOnly, Category="HUD")
	TSubclassOf<UClassicHUDSlotWidget> ItemSlotWidgetClass;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UClassicHUDSlotWidget>> AbilityWidgets;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UClassicHUDSlotWidget>> ItemWidgets;

	TWeakObjectPtr<AHeroCharacter> BoundHero;
	TWeakObjectPtr<AParticipantState> BoundParticipant;
	TWeakObjectPtr<UHeroAbilitySystemComponent> BoundAbilities;
	TWeakObjectPtr<UHeroAttributeComponent> BoundAttributes;
	TWeakObjectPtr<UWorld> BoundWorld;
	TArray<TPair<FGameplayAttribute, FDelegateHandle>> AttributeHandles;
	FTimerHandle BindingTimer;
	FTimerHandle RefreshTimer;
	FTimerHandle CooldownTimer;
	FStatusPanelSnapshot PresentedStatus;
	uint32 BoundEpoch = 0;
	uint8 BoundProfile = MAX_uint8;
	bool bConstructed = false;
};
