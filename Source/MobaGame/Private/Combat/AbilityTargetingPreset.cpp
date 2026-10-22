#include "Combat/AbilityTargetingPreset.h"

#include "Combat/AbilityTargetingTasks.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

UAbilityTargetingPreset::UAbilityTargetingPreset(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TargetingTaskSet.Tasks = {
		CreateDefaultSubobject<UAbilityTargetSelectionTask>(TEXT("Selection")),
		CreateDefaultSubobject<UCombatTargetFilterTask>(TEXT("CombatFilter")),
		CreateDefaultSubobject<UAbilityTargetSortTask>(TEXT("SortAndLimit"))};
}

#if WITH_EDITOR
EDataValidationResult UAbilityTargetingPreset::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	if (!Geometry.IsValid() || TargetingTaskSet.Tasks.Num() != 3
		|| !IsValid(TargetingTaskSet.Tasks[0]) || !TargetingTaskSet.Tasks[0]->IsA<UAbilityTargetSelectionTask>()
		|| !IsValid(TargetingTaskSet.Tasks[1]) || !TargetingTaskSet.Tasks[1]->IsA<UCombatTargetFilterTask>()
		|| !IsValid(TargetingTaskSet.Tasks[2]) || !TargetingTaskSet.Tasks[2]->IsA<UAbilityTargetSortTask>())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "TargetingPreset", "目标配置必须有效，并按选择、战斗过滤、排序限量顺序配置三个任务。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

FAbilityTargetGeometry UAbilityTargetingPreset::GetGeometry(const FCombatRules* CombatRules) const
{
	FAbilityTargetGeometry TargetGeometry = Geometry;
	if (bUseMatchNeutralTargetPolicy && CombatRules && CombatRules->bAllowNeutralTargets)
	{
		TargetGeometry.AllowedRelations.AddUnique(ETeamRelation::Neutral);
	}

	return TargetGeometry;
}
