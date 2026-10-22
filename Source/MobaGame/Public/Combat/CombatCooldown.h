#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "GameplayModMagnitudeCalculation.h"
#include "CombatCooldown.generated.h"

// 仅技能冷却使用；物品与普攻间隔维持各自原生期限。
UCLASS()
class MOBAGAME_API UAbilityCooldownParametersComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	UPROPERTY(EditDefaultsOnly, Category="Cooldown")
	FScalableFloat BaseDuration;
};

UCLASS()
class MOBAGAME_API UAbilityCooldownMagnitudeCalculation : public UGameplayModMagnitudeCalculation
{
	GENERATED_BODY()

public:
	UAbilityCooldownMagnitudeCalculation();

	virtual float CalculateBaseMagnitude_Implementation(const FGameplayEffectSpec& Spec) const override;
};
