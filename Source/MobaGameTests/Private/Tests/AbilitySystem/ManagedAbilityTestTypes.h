#pragma once

#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayEffectCustomApplicationRequirement.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "ManagedAbilityTestTypes.generated.h"

// DeveloperTool 夹具：真实执行 GAS 激活、结束与效果应用，不加载内容资产。
UCLASS(Transient, NotBlueprintable)
class UManagedAbilityTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UManagedAbilityTestAbility();

	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityHoldingAbility : public UManagedAbilityTestAbility
{
	GENERATED_BODY()

public:
	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;


public:
	void Finish();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityUncancelableAbility : public UManagedAbilityHoldingAbility
{
	GENERATED_BODY()

public:
	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityPassiveEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityPassiveEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityCooldownEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityRejectRequirement : public UGameplayEffectCustomApplicationRequirement
{
	GENERATED_BODY()

public:
	virtual bool CanApplyGameplayEffect_Implementation(const UGameplayEffect* GameplayEffect, const FGameplayEffectSpec& Spec, UAbilitySystemComponent* ASC) const override;


public:
	static int32 ApplicationAttempts;
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityRejectedEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityRejectedEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityEventHoldingAbility : public UManagedAbilityUncancelableAbility
{
	GENERATED_BODY()

public:
	UManagedAbilityEventHoldingAbility();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityPrimaryCostEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityPrimaryCostEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityAdditionalEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityAdditionalEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityRemoveOtherEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityRemoveOtherEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityCancelEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityCancelEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityGrantAbilityEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityGrantAbilityEffect();
};

// 白名单原生组件的自定义派生同样不应进入来源事务。
UCLASS(Transient, NotBlueprintable)
class UManagedAbilityCustomTagsComponent : public UTargetTagsGameplayEffectComponent
{
	GENERATED_BODY()

public:
	virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer& Container, FGameplayEffectSpec& Spec, FPredictionKey& PredictionKey) const override;


public:
	static int32 ApplicationCount;
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityCustomComponentEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityCustomComponentEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityCustomRequirementEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityCustomRequirementEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityRemovalRequirementEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityRemovalRequirementEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityOngoingRequirementEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityOngoingRequirementEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityGrantedTagEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UManagedAbilityGrantedTagEffect();
};

UCLASS(Transient, NotBlueprintable)
class UManagedAbilityOwnedTagTestAbility : public UManagedAbilityTestAbility
{
	GENERATED_BODY()

public:
	UManagedAbilityOwnedTagTestAbility();

	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;


public:
	static int32 ActivationCount;
};
