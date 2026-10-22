#pragma once

#include "Abilities/GameplayAbility.h"
#include "CombatCancellationTestAbility.generated.h"

// 仅原生 Automation fixture 使用，不进入内容授予集合。
UCLASS(Transient, NotBlueprintable)
class UCombatCancellationTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UCombatCancellationTestAbility();

	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
