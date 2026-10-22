#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "CombatStateEffectComponent.generated.h"

// 组件只保存只读策略；实例寿命由 ActiveGE 持有。
UCLASS()
class MOBAGAME_API UCombatStateEffectComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
	virtual bool CanGameplayEffectApply(const FActiveGameplayEffectsContainer& Container, const FGameplayEffectSpec& Spec) const override;

	virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer& Container, FGameplayEffectSpec& Spec, FPredictionKey& Key) const override;
};
