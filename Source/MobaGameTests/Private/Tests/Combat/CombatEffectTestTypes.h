#pragma once
#include "Combat/CombatStatusEffects.h"
#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"
#include "Combat/TargetDamageAbility.h"
#include "CombatEffectTestTypes.generated.h"

UCLASS(Transient, NotBlueprintable)
class UInvalidCombatShieldEffect : public UCombatShieldEffect
{
	GENERATED_BODY()

public:
	UInvalidCombatShieldEffect();

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

UCLASS(Transient, NotBlueprintable)
class URejectedCombatShieldEffect : public UCombatShieldEffect
{
	GENERATED_BODY()

public:
	URejectedCombatShieldEffect();
};

UCLASS(Transient, NotBlueprintable)
class UCostedSelfCombatTestAbility : public USelfCombatEffectAbility
{
	GENERATED_BODY()

public:
	UCostedSelfCombatTestAbility();
};

UCLASS(Transient, NotBlueprintable)
class UCommitRejectedShieldEffect : public UCombatShieldEffect
{
	GENERATED_BODY()

public:
	UCommitRejectedShieldEffect();
};

UCLASS(Transient, NotBlueprintable)
class UInvalidDurationShieldEffect : public UCombatShieldEffect
{
	GENERATED_BODY()

public:
	UInvalidDurationShieldEffect();

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

UCLASS(Transient, NotBlueprintable)
class UCombatEquipmentTestEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UCombatEquipmentTestEffect();
};

UCLASS(Transient, NotBlueprintable)
class UCombatHoldingTestAbility : public UManagedAbilityHoldingAbility
{
	GENERATED_BODY()

public:
	UCombatHoldingTestAbility();
};

UCLASS(Transient, NotBlueprintable)
class UGuaranteedCriticalTestEffect : public UCombatDamageEffect
{
	GENERATED_BODY()

public:
	UGuaranteedCriticalTestEffect();
};

UCLASS(Transient, NotBlueprintable)
class UGuaranteedCriticalProjectileTestAbility : public UTargetDamageAbility
{
	GENERATED_BODY()

public:
	UGuaranteedCriticalProjectileTestAbility();
};
