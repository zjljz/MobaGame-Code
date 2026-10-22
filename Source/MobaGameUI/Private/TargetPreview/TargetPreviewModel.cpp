#include "TargetPreview/TargetPreviewModel.h"

#include "MobaGameplayTags.h"

#define LOCTEXT_NAMESPACE "MobaTargetPreview"

void FTargetPreviewSelection::Update(const FClientTargetContext& NewContext, const TArray<FTargetPreviewSlot>& NewSlots)
{
	if (!Context.HasSameBinding(NewContext))
	{
		Reset();
	}
	else if (!Context.HasSamePresentation(NewContext) || Slots != NewSlots)
	{
		SelectedSlot = FGameplayTag();
	}

	Context = NewContext;
	Slots = NewContext.bValid && NewContext.bReady && NewContext.bAlive ? NewSlots : TArray<FTargetPreviewSlot>();
	if (!NewContext.bValid || !NewContext.bReady || !NewContext.bAlive)
	{
		SelectedSlot = FGameplayTag();
		return;
	}

	if (bInitialSelectionPending)
	{
		Select(MobaGameplayTags::Input_Slot_Target);
		bInitialSelectionPending = false;
	}
}

bool FTargetPreviewSelection::Select(FGameplayTag Slot)
{
	if (!Slot.IsValid())
	{
		SelectedSlot = FGameplayTag();
		bInitialSelectionPending = false;
		return true;
	}

	const FTargetPreviewSlot* Entry = Slots.FindByPredicate([Slot](const FTargetPreviewSlot& Value) { return Value.Slot == Slot; });
	if (!Context.bValid || !Context.bReady || !Context.bAlive || !Entry || !Entry->bEnabled)
	{
		return false;
	}

	SelectedSlot = Slot;
	bInitialSelectionPending = false;
	return true;
}

void FTargetPreviewSelection::Reset()
{
	Context = {};
	Slots.Reset();
	SelectedSlot = FGameplayTag();
	bInitialSelectionPending = true;
}

bool MobaTargetPreview::BuildOutline(const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent, TArray<FVector>& Points)
{
	Points.Reset();
	FString Reason;
	if (!MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason))
	{
		return false;
	}

	const FVector Direction = Intent.Direction.GetSafeNormal2D();
	if (Geometry.Policy == EAbilityTargetPolicy::Line)
	{
		const FVector Side(-Direction.Y * Geometry.HalfWidth, Direction.X * Geometry.HalfWidth, 0);
		const FVector End = Origin + Direction * Geometry.Range;
		Points = {Origin + Side, End + Side, End - Side, Origin - Side, Origin + Side};
		return true;
	}

	if (Geometry.Policy == EAbilityTargetPolicy::Cone)
	{
		constexpr int32 ArcSegments = 32;
		Points.Reserve(ArcSegments + 3);
		Points.Add(Origin);
		for (int32 Index = 0; Index <= ArcSegments; ++Index)
		{
			const float Angle = FMath::Lerp(-Geometry.HalfAngleDegrees, Geometry.HalfAngleDegrees, float(Index) / ArcSegments);
			Points.Add(Origin + Direction.RotateAngleAxis(Angle, FVector::UpVector) * Geometry.Range);
		}

		Points.Add(Origin);
		return true;
	}

	return false;
}

FText MobaTargetPreview::GetPolicyText(EAbilityTargetPolicy Policy)
{
	switch (Policy)
	{
	case EAbilityTargetPolicy::None: return LOCTEXT("None", "无目标");
	case EAbilityTargetPolicy::Self: return LOCTEXT("Self", "自身");
	case EAbilityTargetPolicy::Unit: return LOCTEXT("Unit", "单位");
	case EAbilityTargetPolicy::Point: return LOCTEXT("Point", "地点");
	case EAbilityTargetPolicy::Direction: return LOCTEXT("Direction", "方向");
	case EAbilityTargetPolicy::Area: return LOCTEXT("Area", "区域");
	case EAbilityTargetPolicy::Line: return LOCTEXT("Line", "矩形");
	case EAbilityTargetPolicy::Cone: return LOCTEXT("Cone", "扇形");
	default: return LOCTEXT("Unknown", "未知目标类型");
	}
}

#undef LOCTEXT_NAMESPACE
