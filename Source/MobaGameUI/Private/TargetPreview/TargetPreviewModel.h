#pragma once

#include "CoreMinimal.h"
#include "TargetPreview/ClientTargetContext.h"
#include "Combat/AbilityTargeting.h"

/** 本地选槽寿命；从不激活能力或更改服务器槽映射。 */
class FTargetPreviewSelection
{
public:
	void Update(const FClientTargetContext& NewContext, const TArray<FTargetPreviewSlot>& NewSlots);

	bool Select(FGameplayTag Slot);

	void Reset();


public:
	FGameplayTag GetSelectedSlot() const { return SelectedSlot; }
	const TArray<FTargetPreviewSlot>& GetSlots() const { return Slots; }


private:
	FClientTargetContext Context;
	TArray<FTargetPreviewSlot> Slots;
	FGameplayTag SelectedSlot;

	// 新绑定或 Reset 后，在首次有效、就绪且存活的更新中尝试默认槽一次；该尝试或显式选择成功/清除后结束，不等待默认槽选择成功。
	bool bInitialSelectionPending = true;
};

namespace MobaTargetPreview
{
	/** 仅生成新增 Line/Cone 的 XY 闭合边界；失败或其他 policy 必须清空上帧输出。 */
	bool BuildOutline(const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent, TArray<FVector>& Points);

	FText GetPolicyText(EAbilityTargetPolicy Policy);
}

