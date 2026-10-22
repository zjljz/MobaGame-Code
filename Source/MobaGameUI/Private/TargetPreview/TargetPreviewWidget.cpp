#include "TargetPreview/TargetPreviewWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "MobaLogChannels.h"
#include "TargetPreview/PreviewSlotWidget.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Combat/TargetDamageAbility.h"
#include "Hero/HeroCharacter.h"
#include "Layout/Clipping.h"
#include "MobaGameplayTags.h"
#include "Player/HeroPlayerController.h"
#include "Rendering/DrawElements.h"

#define LOCTEXT_NAMESPACE "MobaTargetPreviewWidget"

namespace
{
	FText GetSlotLabel(FGameplayTag Slot)
	{
		if (Slot == MobaGameplayTags::Input_Slot_Primary)
		{
			return LOCTEXT("Primary", "主技能");
		}

		if (Slot == MobaGameplayTags::Input_Slot_UnitAttack)
		{
			return LOCTEXT("Unit", "单位技能");
		}

		if (Slot == MobaGameplayTags::Input_Slot_Target)
		{
			return LOCTEXT("Target", "目标技能");
		}

		return FText::FromName(Slot.GetTagName());
	}
}

UTargetPreviewWidget::UTargetPreviewWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(false);
}

void UTargetPreviewWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	ClearButton->OnClicked.AddUniqueDynamic(this, &ThisClass::ClearSelection);
}

void UTargetPreviewWidget::NativeConstruct()
{
	Super::NativeConstruct();
	ClearPreview();
	RefreshPreview();
	SummaryText->SetText(Summary);
}

void UTargetPreviewWidget::NativeDestruct()
{
	ClearPreview();
	if (SlotButtons)
	{
		SlotButtons->ClearChildren();
	}

	SlotEntries.Reset();
	DisplayedSlots.Reset();
	SummaryText->SetText(FText::GetEmpty());
	Super::NativeDestruct();
}

void UTargetPreviewWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshPreview();
	SummaryText->SetText(Summary);
}

int32 UTargetPreviewWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 BaseLayer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	AHeroPlayerController* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	if (!Controller || Outline.Num() < 2 || !Context.bReady || !Context.HasSamePresentation(MobaClientTarget::ReadContext(Controller)))
	{
		return BaseLayer;
	}

	OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry));
	for (int32 Index = 1; Index < Outline.Num(); ++Index)
	{
		FVector2D ScreenA, ScreenB;
		if (!Controller->ProjectWorldLocationToScreen(Outline[Index - 1], ScreenA, false)
			|| !Controller->ProjectWorldLocationToScreen(Outline[Index], ScreenB, false))
		{
			continue;
		}

		FVector2D LocalA, LocalB;
		// NativePaint的geometry位于窗口绘制空间；移除缓存viewport几何的桌面窗口偏移后再转局部坐标。
		USlateBlueprintLibrary::ScreenToWidgetLocal(Controller, AllottedGeometry, ScreenA, LocalA, true);
		USlateBlueprintLibrary::ScreenToWidgetLocal(Controller, AllottedGeometry, ScreenB, LocalB, true);
		if (!FMath::IsFinite(LocalA.X) || !FMath::IsFinite(LocalA.Y) || !FMath::IsFinite(LocalB.X) || !FMath::IsFinite(LocalB.Y)
			|| FMath::Abs(LocalA.X) > MAX_flt || FMath::Abs(LocalA.Y) > MAX_flt || FMath::Abs(LocalB.X) > MAX_flt || FMath::Abs(LocalB.Y) > MAX_flt)
		{
			continue;
		}

		TArray<FVector2f> Segment = {FVector2f(LocalA), FVector2f(LocalB)};
		FSlateDrawElement::MakeLines(OutDrawElements, BaseLayer + 1, AllottedGeometry.ToPaintGeometry(), MoveTemp(Segment),
			ESlateDrawEffect::None, FLinearColor(0.1f, 0.85f, 1.0f) * InWidgetStyle.GetColorAndOpacityTint(), true, 2.0f);
	}

	OutDrawElements.PopClip();
	return BaseLayer + 1;
}

void UTargetPreviewWidget::ChooseSlot(FGameplayTag PreviewSlot)
{
	// 点击时重新验证映射；不让上一帧已删除的按钮选中失效 Spec。
	RefreshPreview();
	Selection.Select(PreviewSlot);
	RefreshPreview();
	SummaryText->SetText(Summary);
}

void UTargetPreviewWidget::RefreshPreview()
{
	Outline.Reset();
	AHeroPlayerController* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	Context = MobaClientTarget::ReadContext(Controller);
	const TArray<FTargetPreviewSlot> Slots = MobaClientTarget::ReadSlots(Context);
	Selection.Update(Context, Slots);
	RebuildSlotButtons();

	if (!Context.bValid)
	{
		Summary = LOCTEXT("Unbound", "等待本地英雄绑定");
		return;
	}

	if (!Context.bAlive)
	{
		Summary = LOCTEXT("Dead", "死亡期间不显示目标预览");
		return;
	}

	if (!Context.bReady)
	{
		Summary = LOCTEXT("Sync", "同步中");
		return;
	}

	const FGameplayTag PreviewSlot = Selection.GetSelectedSlot();
	UHeroAbilitySystemComponent* HeroASC = Context.Abilities.Get();
	const FGameplayAbilitySpec* Spec = HeroASC->FindAbilitySpecFromHandle(HeroASC->FindSlot(PreviewSlot));
	if (!Spec || !Spec->Ability || !HeroASC->IsManagedAbilityEnabled(Spec->Handle))
	{
		Selection.Select(FGameplayTag());
		Summary = LOCTEXT("Select", "请选择一个可用预览槽");
		return;
	}

	const UTargetDamageAbility* Ability = Cast<UTargetDamageAbility>(Spec->Ability);
	if (!Ability)
	{
		Summary = FText::Format(LOCTEXT("NoGeometry", "{0}：无目标几何"), GetSlotLabel(PreviewSlot));
		return;
	}

	const FAbilityTargetGeometry Geometry = Ability->GetGeometry();
	FAbilityTargetIntent Intent;
	if (!Controller->BuildCursorIntent(Geometry, Intent))
	{
		Summary = FText::Format(LOCTEXT("NoCursor", "{0}：无法取得鼠标目标"), GetSlotLabel(PreviewSlot));
		return;
	}

	const FVector Origin = Context.Hero->GetActorLocation();
	FString Reason;
	const bool bGeometryValid = MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason);
	Summary = FText::Format(LOCTEXT("Summary", "{0} · {1} · 范围 {2}\n{3}"), GetSlotLabel(PreviewSlot), MobaTargetPreview::GetPolicyText(Geometry.Policy),
		FText::AsNumber(Geometry.Range), bGeometryValid ? LOCTEXT("Valid", "本地几何有效") : LOCTEXT("Invalid", "本地几何无效"));
	if (bGeometryValid)
	{
		MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline);
	}
}

void UTargetPreviewWidget::RebuildSlotButtons()
{
	if (!SlotButtons)
	{
		return;
	}

	if (!SlotButtonClass || SlotButtonClass->HasAnyClassFlags(CLASS_Abstract))
	{
		if (!bReportedMissingSlotClass)
		{
			UE_LOG(LogMobaControl, Error, TEXT("UIConfigurationInvalid Widget=%s Property=SlotButtonClass"), *GetClass()->GetPathName());
			bReportedMissingSlotClass = true;
		}

		return;
	}

	TArray<FGameplayTag> Slots;
	for (const FTargetPreviewSlot& Entry : Selection.GetSlots())
	{
		Slots.Add(Entry.Slot);
	}

	if (Slots != DisplayedSlots)
	{
		SlotButtons->ClearChildren();
		SlotEntries.Reset();
		DisplayedSlots = Slots;
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			UPreviewSlotWidget* SlotWidget = CreateWidget<UPreviewSlotWidget>(GetOwningPlayer(), SlotButtonClass);
			if (!SlotWidget)
			{
				UE_LOG(LogMobaControl, Error, TEXT("UIEntryCreationFailed Widget=%s Property=SlotButtonClass"), *GetClass()->GetPathName());
				SlotButtons->ClearChildren();
				SlotEntries.Reset();
				return;
			}

			SlotButtons->AddChildToVerticalBox(SlotWidget);
			SlotEntries.Add(SlotWidget);
		}
	}

	for (int32 Index = 0; Index < SlotEntries.Num(); ++Index)
	{
		const FTargetPreviewSlot& Entry = Selection.GetSlots()[Index];
		SlotEntries[Index]->PresentSlot(this, Entry.Slot, GetSlotLabel(Entry.Slot), Entry.bEnabled, Entry.Slot == Selection.GetSelectedSlot());
	}
}

void UTargetPreviewWidget::ClearSelection()
{
	ChooseSlot(FGameplayTag());
}

void UTargetPreviewWidget::ClearPreview()
{
	Context = {};
	Selection.Reset();
	Outline.Reset();
	Summary = FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE

