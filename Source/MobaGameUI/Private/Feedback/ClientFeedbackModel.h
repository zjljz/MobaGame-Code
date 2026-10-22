#pragma once

#include "CoreMinimal.h"
#include "Items/ItemInventoryComponent.h"
#include "TargetPreview/ClientTargetContext.h"

/** 只消费 Controller 已公开的最近结果；不是逐请求队列，也不推断 Operation/技能槽。 */
class FClientFeedbackModel
{
public:
	void Update(const FClientTargetContext& Context, uint32 IntentSequence, bool bIntentAccepted, FName IntentReason, const FItemResult& ItemResult, double Now);

	void Reset();


private:
	// 通道显示与失效
	void ClearMessages();

	void SetIntentResult(bool bAccepted, FName Reason, double Now);

	void SetItemResult(bool bAccepted, FName Reason, double Now);


public:
	const FText& GetIntentText() const { return IntentText; }
	const FText& GetItemText() const { return ItemText; }
	uint32 GetIntentHighWater() const { return IntentHighWater; }
	uint32 GetItemHighWater() const { return ItemHighWater; }


private:
	FClientTargetContext PreviousContext;
	FText IntentText;
	FText ItemText;
	uint32 IntentHighWater = 0;
	uint32 ItemHighWater = 0;
	double IntentExpiresAt = 0;
	double ItemExpiresAt = 0;
	bool bSeeded = false;
};

namespace MobaClientFeedback
{
	FText DescribeReason(FName Reason);
}
