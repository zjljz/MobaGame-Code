#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "Combat/CombatRules.h"
#include "CombatEffectParameters.generated.h"

// 只读内容合同；不在组件中保存施放、目标或活动效果状态。
UCLASS()
class MOBAGAME_API UCombatDamageParametersComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	bool Resolve(const FGameplayEffectSpec& Spec, FCombatDamageParameters& Parameters) const;


public:
	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	FGameplayEffectModifierMagnitude BaseDamage = FScalableFloat(100);

	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	bool bCanCritical = false;

	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	FScalableFloat AttackDamageCoefficient;

	UPROPERTY(EditDefaultsOnly, Category = "Damage")
	FScalableFloat AbilityPowerCoefficient;
};

UCLASS()
class MOBAGAME_API UCombatHealingParametersComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	UPROPERTY(EditDefaultsOnly, Category = "Healing")
	FGameplayEffectModifierMagnitude HealAmount = FScalableFloat(25);

	UPROPERTY(EditDefaultsOnly, Category = "Healing")
	FScalableFloat AttackDamageCoefficient;

	UPROPERTY(EditDefaultsOnly, Category = "Healing")
	FScalableFloat AbilityPowerCoefficient;
};

// 单池护盾的数值和生命周期；ActiveGE handle 是运行态身份。
UCLASS()
class MOBAGAME_API UCombatShieldEffectComponent : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

	virtual bool CanGameplayEffectApply(const FActiveGameplayEffectsContainer& Container, const FGameplayEffectSpec& Spec) const override;

	virtual bool OnActiveGameplayEffectAdded(FActiveGameplayEffectsContainer& Container, FActiveGameplayEffect& Effect) const override;


public:
	UPROPERTY(EditDefaultsOnly, Category = "Shield")
	FGameplayEffectModifierMagnitude ShieldAmount = FScalableFloat(30);

	UPROPERTY(EditDefaultsOnly, Category = "Shield")
	FScalableFloat AttackDamageCoefficient;

	UPROPERTY(EditDefaultsOnly, Category = "Shield")
	FScalableFloat AbilityPowerCoefficient;
};

namespace MobaCombat
{
	/** 只支持原生静态/等级曲线或显式Tag SetByCaller；缺参和非有限值失败，不做默认回退。 */
	MOBAGAME_API bool ResolveMagnitude(const FGameplayEffectModifierMagnitude& Magnitude, const FGameplayEffectSpec& Spec, float& Value);

	/** Self事务只接收可预检的治疗或单池盾结构；不允许未知回调、动态输入或额外结算。 */
	MOBAGAME_API bool ValidateSelfEffect(const UGameplayEffect* Effect, float Level, bool& bHealing, float& Amount);
}
