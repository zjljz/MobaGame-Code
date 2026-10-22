#include "Status/ClassicHUDSlotWidget.h"

#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Player/HeroPlayerController.h"

#define LOCTEXT_NAMESPACE "ClassicHUDSlot"

void UClassicHUDSlotWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(false);
	ActionButton->OnClicked.AddUniqueDynamic(this, &ThisClass::UseItem);
}

void UClassicHUDSlotWidget::NativeDestruct()
{
	ActionButton->OnClicked.RemoveDynamic(this, &ThisClass::UseItem);
	PresentedItem.Invalidate();
	Super::NativeDestruct();
}

void UClassicHUDSlotWidget::Present(const FText& Name, const FText& Detail, const FText& Key, bool bEnabled, FGuid ItemInstance)
{
	PresentedItem = ItemInstance;
	NameText->SetText(Name);
	DetailText->SetText(Detail);
	KeyText->SetText(Key);
	SetToolTipText(FText::Format(LOCTEXT("Tooltip", "{0}\n{1}"), Name, Detail));
	// 没有鼠标施法映射的技能格穿透；物品按钮也不获取键盘焦点。
	ActionButton->SetVisibility(ItemInstance.IsValid() ? ESlateVisibility::Visible : ESlateVisibility::HitTestInvisible);
	ActionButton->SetIsEnabled(bEnabled);
	NameText->SetRenderOpacity(bEnabled ? 1.f : .5f);
}

void UClassicHUDSlotWidget::PresentCooldown(bool bKnown, float Remaining, float Duration)
{
	const bool bActive = (bKnown && FMath::IsFinite(Remaining) && Remaining > 0);
	CooldownText->SetText(!bKnown ? LOCTEXT("Pending", "—")
		: (bActive ? FText::AsNumber(FMath::CeilToInt(Remaining)) : FText::GetEmpty()));
	CooldownMask->SetPercent(bActive && FMath::IsFinite(Duration) && Duration > 0 ? FMath::Clamp(Remaining / Duration, 0.f, 1.f) : 0.f);
	CooldownMask->SetVisibility(bActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UClassicHUDSlotWidget::UseItem()
{
	if (auto* Controller = Cast<AHeroPlayerController>(GetOwningPlayer()); Controller && PresentedItem.IsValid())
	{
		Controller->MobaUseItem(PresentedItem.ToString());
	}
}

#undef LOCTEXT_NAMESPACE
