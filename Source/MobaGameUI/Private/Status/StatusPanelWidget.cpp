#include "Status/StatusPanelWidget.h"

#include "Components/HorizontalBox.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "MobaLogChannels.h"
#include "Player/HeroPlayerController.h"
#include "Status/StatusTextWidget.h"

#define LOCTEXT_NAMESPACE "MobaStatusPanel"

namespace
{
	FText StatusNumber(float Value)
	{
		FNumberFormattingOptions Options;
		Options.SetMaximumFractionalDigits(1);
		return FText::AsNumber(Value, &Options);
	}

	FText AbilityText(const FStatusAbilityRow& Row, EHeroLifeState LifeState)
	{
		FString SlotName = Row.Slot.ToString();
		int32 LastDot = INDEX_NONE;
		if (SlotName.FindLastChar(TEXT('.'), LastDot))
		{
			SlotName.RightChopInline(LastDot + 1);
		}

		const FText State = !Row.bEnabled ? LOCTEXT("Disabled", "禁用")
			: (LifeState != EHeroLifeState::Alive ? LOCTEXT("CannotCastDead", "当前生命状态不可施放") : LOCTEXT("Enabled", "启用"));
		const FText Cooldown = !Row.bCooldownKnown ? LOCTEXT("CooldownSync", "冷却同步中")
			: (Row.CooldownRemaining > 0 ? FText::Format(LOCTEXT("CooldownSeconds", "冷却 {0} 秒"), StatusNumber(Row.CooldownRemaining)) : LOCTEXT("CooldownReady", "冷却就绪"));
		return FText::Format(LOCTEXT("AbilityRow", "{0}  ·  {1}\n等级 {2}  |  {3}  |  {4}"),
			FText::FromString(SlotName), Row.Name, FText::AsNumber(Row.Level), State, Cooldown);
	}
}

UStatusPanelWidget::UStatusPanelWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UStatusPanelWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::HitTestInvisible);
	bPresentationReady = (StatusCard && LifeText && HealthText && ManaText && GoldText && HealthBar && ManaBar && AbilityBox && ItemBox
		&& AbilityRowClass && !AbilityRowClass->HasAnyClassFlags(CLASS_Abstract)
		&& ItemSlotClass && !ItemSlotClass->HasAnyClassFlags(CLASS_Abstract) && GetOwningPlayer());
	if (!bPresentationReady)
	{
		UE_LOG(LogMobaControl, Error, TEXT("StatusWidgetInvalid Class=%s Reason=MissingBindingOrEntryClassOrPlayer"), *GetClass()->GetPathName());
		return;
	}

	ClearEntries();
	RefreshStatus();
}

void UStatusPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshStatus();
}

void UStatusPanelWidget::NativeDestruct()
{
	PresentStatus(FStatusPanelSnapshot());
	ClearEntries();
	bPresentationReady = false;
	Super::NativeDestruct();
}

void UStatusPanelWidget::RefreshStatus()
{
	const UWorld* World = GetWorld();
	const UExperienceSubsystem* ExperienceSubsystem = World ? World->GetSubsystem<UExperienceSubsystem>() : nullptr;
	const UMatchRulesDefinition* MatchRulesDef = ExperienceSubsystem && ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady ? ExperienceSubsystem->GetRules() : nullptr;
	PresentStatus(MobaStatus::ReadOwnedStatus(Cast<AHeroPlayerController>(GetOwningPlayer()), MatchRulesDef));
}

void UStatusPanelWidget::PresentStatus(const FStatusPanelSnapshot& Snapshot)
{
	PresentedStatus = Snapshot;
	if (!bPresentationReady)
	{
		return;
	}

	const FText Sync = LOCTEXT("Syncing", "同步中");
	LifeText->SetText(!Snapshot.bOwnedHero ? LOCTEXT("WaitingHero", "等待自己的英雄 · 同步中")
		: (!Snapshot.bCombatReady ? LOCTEXT("LifeSync", "战斗状态 · 同步中")
			: (Snapshot.LifeState == EHeroLifeState::Dead ? FText::Format(LOCTEXT("Dead", "已阵亡 · 复活 {0} 秒"), StatusNumber(Snapshot.RespawnRemaining)) : LOCTEXT("Alive", "英雄状态 · 存活"))));
	HealthText->SetText(Snapshot.bCombatReady
		? FText::Format(LOCTEXT("Health", "HP  {0} / {1}     护盾  {2}"), StatusNumber(Snapshot.Health), StatusNumber(Snapshot.MaxHealth), StatusNumber(Snapshot.Shield))
		: LOCTEXT("HealthSync", "HP / 护盾 · 同步中"));
	ManaText->SetText(!Snapshot.bCombatReady ? LOCTEXT("PrimarySync", "Mana · 同步中")
		: (!Snapshot.bUsesMana ? LOCTEXT("NoResource", "Mana · 无资源") : FText::Format(LOCTEXT("Mana", "Mana  {0} / {1}"), StatusNumber(Snapshot.Mana), StatusNumber(Snapshot.MaxMana))));
	ManaBar->SetVisibility(Snapshot.bCombatReady && !Snapshot.bUsesMana ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	ManaText->SetVisibility(Snapshot.bCombatReady && !Snapshot.bUsesMana ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	HealthBar->SetPercent(Snapshot.bCombatReady ? Snapshot.Health / Snapshot.MaxHealth : 0.f);
	ManaBar->SetPercent(Snapshot.bCombatReady && Snapshot.MaxMana > 0 ? Snapshot.Mana / Snapshot.MaxMana : 0.f);
	GoldText->SetText(Snapshot.bInventoryReady ? FText::Format(LOCTEXT("Gold", "金币  {0}"), FText::AsNumber(Snapshot.Gold)) : LOCTEXT("GoldSync", "金币 / 库存 · 同步中"));

	const int32 RowCount = Snapshot.bAbilitiesReady ? FMath::Max(1, Snapshot.Abilities.Num()) : 1;
	if (!EnsureEntries(RowCount))
	{
		return;
	}

	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		AbilityRows[Index]->SetContent(!Snapshot.bAbilitiesReady ? Sync
			: (Snapshot.Abilities.IsEmpty() ? LOCTEXT("NoSlots", "当前没有技能槽") : AbilityText(Snapshot.Abilities[Index], Snapshot.LifeState)));
	}

	for (int32 SlotIndex = 0; SlotIndex < ItemSlots.Num(); ++SlotIndex)
	{
		const FStatusItemRow& Item = Snapshot.Items[SlotIndex];
		const FText Value = !Snapshot.bInventoryReady ? Sync : (!Snapshot.bItemsEnabled ? LOCTEXT("ItemsDisabled", "未启用")
			: (Item.Instance.IsValid() ? FText::Format(LOCTEXT("Item", "{0}\n×{1}"), FText::FromName(Item.ItemId), FText::AsNumber(Item.Quantity)) : LOCTEXT("Empty", "空")));
		ItemSlots[SlotIndex]->SetContent(FText::Format(LOCTEXT("ItemSlot", "{0}  {1}"), FText::AsNumber(SlotIndex + 1), Value));
	}
}

bool UStatusPanelWidget::EnsureEntries(int32 AbilityRowCount)
{
	while (AbilityRows.Num() > AbilityRowCount)
	{
		AbilityRows.Last()->RemoveFromParent();
		AbilityRows.Pop();
	}

	while (AbilityRows.Num() < AbilityRowCount)
	{
		UStatusTextWidget* AbilityRowWidget = CreateWidget<UStatusTextWidget>(GetOwningPlayer(), AbilityRowClass);
		if (!AbilityRowWidget)
		{
			UE_LOG(LogMobaControl, Error, TEXT("StatusWidgetInvalid Class=%s Reason=AbilityRowCreationFailed"), *GetClass()->GetPathName());
			bPresentationReady = false;
			ClearEntries();
			return false;
		}

		AbilityBox->AddChildToVerticalBox(AbilityRowWidget);
		AbilityRows.Add(AbilityRowWidget);
	}

	while (ItemSlots.Num() < 6)
	{
		UStatusTextWidget* ItemSlotWidget = CreateWidget<UStatusTextWidget>(GetOwningPlayer(), ItemSlotClass);
		if (!ItemSlotWidget)
		{
			UE_LOG(LogMobaControl, Error, TEXT("StatusWidgetInvalid Class=%s Reason=ItemSlotCreationFailed"), *GetClass()->GetPathName());
			bPresentationReady = false;
			ClearEntries();
			return false;
		}

		ItemBox->AddChildToHorizontalBox(ItemSlotWidget);
		ItemSlots.Add(ItemSlotWidget);
	}

	return true;
}

void UStatusPanelWidget::ClearEntries()
{
	if (AbilityBox)
	{
		AbilityBox->ClearChildren();
	}

	if (ItemBox)
	{
		ItemBox->ClearChildren();
	}

	AbilityRows.Reset();
	ItemSlots.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
FStatusPanelVisualMetrics UStatusPanelWidget::GetVisualMetrics() const
{
	FStatusPanelVisualMetrics Metrics;
	if (StatusCard && HealthText)
	{
		const FGeometry& Host = GetCachedGeometry();
		const FGeometry& Panel = StatusCard->GetCachedGeometry();
		Metrics.HostSize = Host.GetAbsoluteSize();
		Metrics.PanelOffset = FVector2D(Panel.GetAbsolutePosition()) - FVector2D(Host.GetAbsolutePosition());
		Metrics.PanelSize = Panel.GetAbsoluteSize();
		Metrics.HostScale = Host.GetAccumulatedLayoutTransform().GetScale();
		Metrics.ReadabilityScale = 1;
		Metrics.BodyLineHeight = static_cast<float>(FVector2D(HealthText->GetCachedGeometry().GetAbsoluteSize()).Y);
		Metrics.bItemTextFits = (ItemSlots.Num() == 6);
		for (const UStatusTextWidget* Item : ItemSlots)
		{
			const FVector2D Available = Item->GetCachedGeometry().GetLocalSize();
			const FVector2D Desired = Item->GetDesiredSize();
			Metrics.bItemTextFits &= (Available.X + 1 >= Desired.X && Available.Y + 1 >= Desired.Y);
		}
	}
	return Metrics;
}
#endif

#undef LOCTEXT_NAMESPACE

