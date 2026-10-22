#include "Feedback/ClientFeedbackModel.h"

#define LOCTEXT_NAMESPACE "MobaClientFeedback"

void FClientFeedbackModel::Update(const FClientTargetContext& Context, uint32 IntentSequence, bool bIntentAccepted, FName IntentReason, const FItemResult& ItemResult, double Now)
{
	if (!Context.bValid || !FMath::IsFinite(Now))
	{
		Reset();
		return;
	}

	if (!bSeeded || !PreviousContext.HasSameBinding(Context))
	{
		Reset();
		PreviousContext = Context;
		IntentHighWater = IntentSequence;
		ItemHighWater = ItemResult.Epoch == Context.Epoch ? ItemResult.Sequence : 0;
		bSeeded = true;
		return;
	}

	if (!PreviousContext.HasSamePresentation(Context))
	{
		ClearMessages();
	}

	PreviousContext = Context;
	if (Now >= IntentExpiresAt)
	{
		IntentText = FText::GetEmpty();
	}

	if (Now >= ItemExpiresAt)
	{
		ItemText = FText::GetEmpty();
	}

	// uint32 自然回绕；超过半个序号空间的差视为旧结果，不让旧快照倒退高水位。
	const uint32 IntentDistance = IntentSequence - IntentHighWater;
	if (IntentDistance != 0 && IntentDistance < 0x80000000u)
	{
		IntentHighWater = IntentSequence;
		SetIntentResult(bIntentAccepted, IntentReason, Now);
	}

	// 物品服务器使用单调高水位且拒绝 0；UI 不自行增加回绕协议。
	if (ItemResult.Epoch == Context.Epoch && ItemResult.Sequence > ItemHighWater)
	{
		ItemHighWater = ItemResult.Sequence;
		SetItemResult(ItemResult.bAccepted, ItemResult.Reason, Now);
	}
}

void FClientFeedbackModel::Reset()
{
	PreviousContext = {};
	IntentHighWater = 0;
	ItemHighWater = 0;
	bSeeded = false;
	ClearMessages();
}

void FClientFeedbackModel::ClearMessages()
{
	IntentText = FText::GetEmpty();
	ItemText = FText::GetEmpty();
	IntentExpiresAt = 0;
	ItemExpiresAt = 0;
}

void FClientFeedbackModel::SetIntentResult(bool bAccepted, FName Reason, double Now)
{
	IntentText = bAccepted ? FText::GetEmpty() : FText::Format(LOCTEXT("IntentRejected", "操作被拒绝：{0}"), MobaClientFeedback::DescribeReason(Reason));
	IntentExpiresAt = bAccepted ? 0 : Now + 3.0;
}

void FClientFeedbackModel::SetItemResult(bool bAccepted, FName Reason, double Now)
{
	ItemText = bAccepted ? FText::GetEmpty() : FText::Format(LOCTEXT("ItemRejected", "物品请求被拒绝：{0}"), MobaClientFeedback::DescribeReason(Reason));
	ItemExpiresAt = bAccepted ? 0 : Now + 3.0;
}

FText MobaClientFeedback::DescribeReason(FName Reason)
{
	if (Reason == TEXT("Cooldown"))
	{
		return LOCTEXT("Cooldown", "正在冷却");
	}

	if (Reason == TEXT("Cost"))
	{
		return LOCTEXT("Cost", "资源不足");
	}

	if (Reason == TEXT("LifeState"))
	{
		return LOCTEXT("LifeState", "当前生命状态不允许此操作");
	}

	if (Reason == TEXT("Disabled") || Reason == TEXT("ActivationBlocked") || Reason == TEXT("NotUsable"))
	{
		return LOCTEXT("Blocked", "当前无法使用");
	}

	if (Reason == TEXT("Slot") || Reason == TEXT("TargetRequired") || Reason == TEXT("Target"))
	{
		return LOCTEXT("Target", "技能槽或目标不可用");
	}

	if (Reason == TEXT("Range") || Reason == TEXT("PointRange") || Reason == TEXT("ProfileRange"))
	{
		return LOCTEXT("Range", "超出允许范围");
	}

	if (Reason == TEXT("LineOfSight"))
	{
		return LOCTEXT("Sight", "目标被遮挡");
	}

	if (Reason == TEXT("Team") || Reason == TEXT("Untargetable") || Reason == TEXT("Invulnerable"))
	{
		return LOCTEXT("TargetRule", "目标不符合当前规则");
	}

	if (Reason == TEXT("Binding") || Reason == TEXT("Entity") || Reason == TEXT("Epoch") || Reason == TEXT("Match"))
	{
		return LOCTEXT("Binding", "控制绑定已变化");
	}

	if (Reason == TEXT("Revision") || Reason == TEXT("InventoryRevision") || Reason == TEXT("NotReady") || Reason == TEXT("EconomyNotReady"))
	{
		return LOCTEXT("Synchronizing", "状态正在同步，请稍后重试");
	}

	if (Reason == TEXT("ShopRange"))
	{
		return LOCTEXT("Shop", "请进入商店范围");
	}

	if (Reason == TEXT("InsufficientGold"))
	{
		return LOCTEXT("Gold", "金币不足");
	}

	if (Reason == TEXT("InventoryFull"))
	{
		return LOCTEXT("Full", "物品栏已满");
	}

	if (Reason == TEXT("FullHealth"))
	{
		return LOCTEXT("Health", "生命值已满");
	}

	if (Reason == TEXT("RecipeRequired") || Reason == TEXT("MissingComponent") || Reason == TEXT("NoRecipe"))
	{
		return LOCTEXT("Recipe", "请检查配方与合成材料");
	}

	if (Reason == TEXT("ItemsDisabled") || Reason == TEXT("ItemNotAllowed") || Reason == TEXT("CatalogUnavailable"))
	{
		return LOCTEXT("Items", "当前物品不可用");
	}

	if (Reason == TEXT("Busy") || Reason == TEXT("ActiveAbility") || Reason == TEXT("TagReactiveContext"))
	{
		return LOCTEXT("Busy", "当前能力或效果状态不允许此操作");
	}

	if (Reason == TEXT("OldSequence") || Reason == TEXT("Instance"))
	{
		return LOCTEXT("Stale", "请求或物品已失效");
	}

	return LOCTEXT("Unknown", "请求未被接受，请重试");
}

#undef LOCTEXT_NAMESPACE
