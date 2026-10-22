#include "Tests/Combat/CombatEffectTestTypes.h"
#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatEffectParameters.h"
#include <limits>

UInvalidCombatShieldEffect::UInvalidCombatShieldEffect()
{
	FindOrAddComponent<UCombatShieldEffectComponent>().ShieldAmount = FScalableFloat(std::numeric_limits<float>::quiet_NaN());
}

#if WITH_EDITOR
EDataValidationResult UInvalidCombatShieldEffect::IsDataValid(FDataValidationContext& Context) const
{
	// 故意非法的瞬态夹具只验证应用拒绝，不作为可编辑资产参与启动校验。
	return EDataValidationResult::NotValidated;
}
#endif

URejectedCombatShieldEffect::URejectedCombatShieldEffect()
{
	auto* Requirement = CreateDefaultSubobject<UTargetTagRequirementsGameplayEffectComponent>(TEXT("Requirement"));
	Requirement->ApplicationTagRequirements.RequireTags.AddTag(MobaGameplayTags::State_Invulnerable);
	GEComponents.Add(Requirement);
}

UCostedSelfCombatTestAbility::UCostedSelfCombatTestAbility()
{
	CostGameplayEffectClass = UManagedAbilityPrimaryCostEffect::StaticClass();
	CooldownGameplayEffectClass = UManagedAbilityCooldownEffect::StaticClass();
	Effect = UCombatShieldEffect::StaticClass();
}

UCommitRejectedShieldEffect::UCommitRejectedShieldEffect()
{
	auto* Requirement = CreateDefaultSubobject<UTargetTagRequirementsGameplayEffectComponent>(TEXT("Requirement"));
	Requirement->ApplicationTagRequirements.IgnoreTags.AddTag(MobaGameplayTags::Cooldown_Self);
	GEComponents.Add(Requirement);
}

UInvalidDurationShieldEffect::UInvalidDurationShieldEffect()
{
	DurationMagnitude = FScalableFloat(0);
}

#if WITH_EDITOR
EDataValidationResult UInvalidDurationShieldEffect::IsDataValid(FDataValidationContext& Context) const
{
	// 故意非法的瞬态夹具只验证应用拒绝，不作为可编辑资产参与启动校验。
	return EDataValidationResult::NotValidated;
}
#endif

UCombatEquipmentTestEffect::UCombatEquipmentTestEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UHeroAttributeSet::GetDamageTakenMultiplierAttribute();
	Modifier.ModifierOp = EGameplayModOp::MultiplyCompound;
	Modifier.ModifierMagnitude = FScalableFloat(.9f);
	FInheritedTagContainer Tags;
	Tags.AddTag(MobaGameplayTags::Effect_Dispel_Undispellable);
	Tags.AddTag(MobaGameplayTags::Effect_Beneficial);
	// 即便被错误同时标Basic，也必须由Undispellable优先保护。
	Tags.AddTag(MobaGameplayTags::Effect_Dispel_Basic);
	auto* Component = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("Tags"));
	GEComponents.Add(Component);
	Component->SetAndApplyTargetTagChanges(Tags);
}

UCombatHoldingTestAbility::UCombatHoldingTestAbility()
{
	CooldownGameplayEffectClass = nullptr;
	FGameplayTagContainer Tags(MobaGameplayTags::Ability_Active);
	Tags.AddTag(MobaGameplayTags::Ability_Spell);
	SetAssetTags(Tags);
}

UGuaranteedCriticalTestEffect::UGuaranteedCriticalTestEffect()
{
	auto& Parameters = FindOrAddComponent<UCombatDamageParametersComponent>();
	Parameters.BaseDamage = FScalableFloat(100);
	Parameters.bCanCritical = true;
	Parameters.CriticalChance = FScalableFloat(1);
	Parameters.CriticalMultiplier = FScalableFloat(1.5f);
}

UGuaranteedCriticalProjectileTestAbility::UGuaranteedCriticalProjectileTestAbility()
{
	DamageEffect = UGuaranteedCriticalTestEffect::StaticClass();
	DamageType = MobaGameplayTags::Damage_Physical;
	Targeting.Policy = EAbilityTargetPolicy::Direction;
	Delivery = EAbilityDelivery::Projectile;
	bLegacyUnitRange = false;
	ProjectileSpeed = 600;
}
