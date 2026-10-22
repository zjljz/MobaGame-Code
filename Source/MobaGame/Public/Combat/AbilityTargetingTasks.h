#pragma once

#include "Tasks/TargetingTask.h"
#include "Tasks/TargetingFilterTask_BasicFilterTemplate.h"
#include "AbilityTargetingTasks.generated.h"

/** 按意图选择自身、指定单位或中心点落在几何内的候选，不处理伤害或投送。 */
UCLASS(EditInlineNew)
class MOBAGAME_API UAbilityTargetSelectionTask : public UTargetingTask
{
	GENERATED_BODY()

public:
	virtual void Execute(const FTargetingRequestHandle& TargetingHandle) const override;
};

/** 复用公共战斗目标规则；几何已在选择阶段检查。 */
UCLASS(EditInlineNew)
class MOBAGAME_API UCombatTargetFilterTask : public UTargetingFilterTask_BasicFilterTemplate
{
	GENERATED_BODY()

protected:
	virtual bool ShouldFilterTarget(const FTargetingRequestHandle& TargetingHandle, const FTargetingDefaultResultData& TargetData) const override;
};

/** 按 XY 距离、EntityId 排序后限制数量，保持现有目标顺序。 */
UCLASS(EditInlineNew)
class MOBAGAME_API UAbilityTargetSortTask : public UTargetingTask
{
	GENERATED_BODY()

public:
	virtual void Execute(const FTargetingRequestHandle& TargetingHandle) const override;
};
