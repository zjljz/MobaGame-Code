#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "CombatHealingExecution.generated.h"

UCLASS()
class MOBAGAME_API UCombatHealingExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UCombatHealingExecution();

	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const override;
};
