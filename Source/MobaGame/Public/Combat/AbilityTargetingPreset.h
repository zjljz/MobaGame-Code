#pragma once

#include "TargetingSystem/TargetingPreset.h"
#include "Combat/AbilityTargeting.h"
#include "AbilityTargetingPreset.generated.h"

/** 只读目标配置；GA 与客户端预览引用同一份几何，任务仅处理单次请求数据。 */
UCLASS()
class MOBAGAME_API UAbilityTargetingPreset : public UTargetingPreset
{
	GENERATED_BODY()

public:
	UAbilityTargetingPreset(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	FAbilityTargetGeometry GetGeometry(const FCombatRules* CombatRules = nullptr) const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FAbilityTargetGeometry Geometry;

	// 该目标配置显式跟随当前比赛的中立目标政策；其他配置只读取 AllowedRelations。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bUseMatchNeutralTargetPolicy = false;
};
