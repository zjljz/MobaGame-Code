#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "CombatDamageExecution.generated.h"

UCLASS()
class MOBAGAME_API UCombatDamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UCombatDamageExecution();

	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const override;
};
