#include "Combat/CombatEffectParameters.h"

#include "Combat/CombatEffectContext.h"

#include "Combat/CombatDamageExecution.h"
#include "Combat/CombatHealingExecution.h"
#include "Combat/CombatantInterface.h"
#include "Combat/HeroAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"
#include "UObject/UObjectHash.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	bool ValidCoefficients(const FScalableFloat& Attack, const FScalableFloat& Power, float Level)
	{
		const float AttackScale = Attack.GetValueAtLevel(Level);
		const float PowerScale = Power.GetValueAtLevel(Level);
		return FMath::IsFinite(AttackScale) && AttackScale >= 0 && FMath::IsFinite(PowerScale) && PowerScale >= 0;
	}
}

#if WITH_EDITOR

namespace
{
	bool IsValidAmount(const FGameplayEffectModifierMagnitude& Magnitude)
	{
		if (Magnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller)
		{
			return Magnitude.GetSetByCallerFloat().DataTag.IsValid();
		}

		float Amount = 0;
		return Magnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::ScalableFloat
			&& Magnitude.GetStaticMagnitudeIfPossible(1, Amount) && FMath::IsFinite(Amount) && Amount >= 0;
	}
}

EDataValidationResult UCombatDamageParametersComponent::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	const auto* Effect = GetOwner();
	const float AttackScale = AttackDamageCoefficient.GetValueAtLevel(1);
	const float AbilityScale = AbilityPowerCoefficient.GetValueAtLevel(1);
	if (!IsValidAmount(BaseDamage) || !FMath::IsFinite(AttackScale) || AttackScale < 0 || !FMath::IsFinite(AbilityScale) || AbilityScale < 0
		|| Effect->Executions.Num() != 1 || Effect->Executions[0].CalculationClass != UCombatDamageExecution::StaticClass()
		|| Effect->FindComponent<UCombatHealingParametersComponent>() || Effect->FindComponent<UCombatShieldEffectComponent>())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "DamageParameters", "伤害组件需要有效基础项、非负系数及唯一伤害Execution。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UCombatDamageParametersComponent::Resolve(const FGameplayEffectSpec& Spec, FCombatDamageParameters& Parameters) const
{
	Parameters.bCanCritical = bCanCritical;
	return MobaCombat::ResolveRawAmount(Spec, BaseDamage, AttackDamageCoefficient, AbilityPowerCoefficient, Parameters.BaseDamage);
}

#if WITH_EDITOR
EDataValidationResult UCombatHealingParametersComponent::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	const auto* Effect = GetOwner();
	if (!IsValidAmount(HealAmount) || !ValidCoefficients(AttackDamageCoefficient, AbilityPowerCoefficient, 1) || Effect->Executions.Num() != 1 || Effect->Executions[0].CalculationClass != UCombatHealingExecution::StaticClass()
		|| Effect->FindComponent<UCombatDamageParametersComponent>() || Effect->FindComponent<UCombatShieldEffectComponent>())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "HealingParameters", "治疗组件需要有效数值及唯一治疗Execution。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

EDataValidationResult UCombatShieldEffectComponent::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	const auto* Effect = GetOwner();
	if (!IsValidAmount(ShieldAmount) || !ValidCoefficients(AttackDamageCoefficient, AbilityPowerCoefficient, 1) || !IsValidAmount(Effect->DurationMagnitude) || Effect->DurationPolicy != EGameplayEffectDurationType::HasDuration || !Effect->Modifiers.IsEmpty()
		|| !Effect->Executions.IsEmpty() || Effect->GetStackingType() != EGameplayEffectStackingType::None || Effect->Period.GetValueAtLevel(1) != 0
		|| !Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_Shield) || !Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_RemoveOnDeath))
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "ShieldParameters", "单池盾需要有效数值、持续期限、护盾/死亡清理标签，且不能有修饰、执行、周期或堆叠。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UCombatShieldEffectComponent::CanGameplayEffectApply(const FActiveGameplayEffectsContainer& Container, const FGameplayEffectSpec& Spec) const
{
	const auto* Actor = Container.Owner ? Container.Owner->GetAvatarActor() : nullptr;
	const auto* TargetCombatant = Cast<ICombatantInterface>(Actor);
	const UGameplayEffect* Effect = Spec.Def;
	float Amount = 0;
	if (!TargetCombatant || !TargetCombatant->IsCombatAlive() || !MobaCombat::ResolveRawAmount(Spec, ShieldAmount, AttackDamageCoefficient, AbilityPowerCoefficient, Amount) || Amount <= 0)
	{
		return false;
	}

	const auto* TargetTagRequirementsComp = Effect->FindComponent<UTargetTagRequirementsGameplayEffectComponent>();
	if (TargetTagRequirementsComp && (!TargetTagRequirementsComp->OngoingTagRequirements.IsEmpty() || !TargetTagRequirementsComp->RemovalTagRequirements.IsEmpty()))
	{
		return false;
	}

	return FMath::IsFinite(Spec.GetDuration()) && Spec.GetDuration() > 0 && Effect->DurationPolicy == EGameplayEffectDurationType::HasDuration
		&& Spec.GetPeriod() == 0 && Effect->GetStackingType() == EGameplayEffectStackingType::None && Effect->Modifiers.IsEmpty() && Effect->Executions.IsEmpty()
		&& Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_Shield) && Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_RemoveOnDeath);
}

bool UCombatShieldEffectComponent::OnActiveGameplayEffectAdded(FActiveGameplayEffectsContainer& Container, FActiveGameplayEffect& Effect) const
{
	if (!Container.IsNetAuthority() || !Effect.Spec.Def->FindComponent<UCombatShieldEffectComponent>())
	{
		return true;
	}

	float Amount = 0;
	if (!MobaCombat::ResolveRawAmount(Effect.Spec, ShieldAmount, AttackDamageCoefficient, AbilityPowerCoefficient, Amount) || Amount <= 0)
	{
		return false;
	}

	auto* ASC = Container.Owner;
	const FActiveGameplayEffectHandle NewShieldEffectHandle = Effect.Handle;
	// 新GE已进入容器；旧移除回调通过原生handle查新盾，不会清掉同帧替换的池。
	Effect.EventSet.OnEffectRemoved.AddWeakLambda(ASC,
		[ASC, NewShieldEffectHandle](const FGameplayEffectRemovalInfo&)
		{
			const auto ShieldEffectHandles = ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Shield)));
			for (const auto ShieldEffectHandle : ShieldEffectHandles)
			{
				const auto* OtherShieldEffect = ASC->GetActiveGameplayEffect(ShieldEffectHandle);
				if (ShieldEffectHandle != NewShieldEffectHandle && OtherShieldEffect && OtherShieldEffect->Spec.Def->FindComponent<UCombatShieldEffectComponent>())
				{
					return;
				}
			}

			ASC->SetNumericAttributeBase(UHeroAttributeSet::GetShieldAttribute(), 0);
		});
	const auto ExistingShieldHandles = ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Shield)));
	for (const auto ShieldEffectHandle : ExistingShieldHandles)
	{
		const auto* OtherShieldEffect = ASC->GetActiveGameplayEffect(ShieldEffectHandle);
		if (ShieldEffectHandle != NewShieldEffectHandle && OtherShieldEffect && OtherShieldEffect->Spec.Def->FindComponent<UCombatShieldEffectComponent>())
		{
			ASC->RemoveActiveGameplayEffect(ShieldEffectHandle);
		}
	}

	// 旧盾移除可重入；只有仍在容器中的本次handle能提交新池。
	if (ASC->GetActiveGameplayEffect(NewShieldEffectHandle))
	{
		ASC->SetNumericAttributeBase(UHeroAttributeSet::GetShieldAttribute(), Amount);
	}

	return true;
}

bool MobaCombat::ResolveMagnitude(const FGameplayEffectModifierMagnitude& Magnitude, const FGameplayEffectSpec& Spec, float& Value)
{
	if (Magnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::ScalableFloat)
	{
		return Magnitude.GetStaticMagnitudeIfPossible(Spec.GetLevel(), Value) && FMath::IsFinite(Value);
	}

	if (Magnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller)
	{
		const FGameplayTag SetByCallerTag = Magnitude.GetSetByCallerFloat().DataTag;
		const float* SuppliedMagnitude = SetByCallerTag.IsValid() ? Spec.SetByCallerTagMagnitudes.Find(SetByCallerTag) : nullptr;
		if (SuppliedMagnitude && FMath::IsFinite(*SuppliedMagnitude))
		{
			Value = *SuppliedMagnitude;
			return true;
		}
	}

	return false;
}

bool MobaCombat::ValidateSelfEffect(const UGameplayEffect* Effect, float Level, bool& bHealing, float& Amount)
{
	if (!Effect || !FMath::IsFinite(Level) || Level <= 0)
	{
		return false;
	}

	const auto* HealingParametersComp = Effect->FindComponent<UCombatHealingParametersComponent>();
	const auto* ShieldEffectComp = Effect->FindComponent<UCombatShieldEffectComponent>();
	bHealing = HealingParametersComp != nullptr;
	if ((HealingParametersComp != nullptr) == (ShieldEffectComp != nullptr))
	{
		return false;
	}

	const auto& Magnitude = HealingParametersComp ? HealingParametersComp->HealAmount : ShieldEffectComp->ShieldAmount;
	const auto& Attack = HealingParametersComp ? HealingParametersComp->AttackDamageCoefficient : ShieldEffectComp->AttackDamageCoefficient;
	const auto& Power = HealingParametersComp ? HealingParametersComp->AbilityPowerCoefficient : ShieldEffectComp->AbilityPowerCoefficient;
	if (!ValidCoefficients(Attack, Power, Level) || Magnitude.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::ScalableFloat
		|| !Magnitude.GetStaticMagnitudeIfPossible(Level, Amount) || !FMath::IsFinite(Amount) || Amount < 0
		|| (Amount == 0 && Attack.GetValueAtLevel(Level) == 0 && Power.GetValueAtLevel(Level) == 0)
		|| Effect->Period.GetValueAtLevel(Level) != 0 || Effect->GetStackingType() != EGameplayEffectStackingType::None || !Effect->Modifiers.IsEmpty()
		|| !Effect->GameplayCues.IsEmpty() || !Effect->GetBlockedAbilityTags().IsEmpty())
	{
		return false;
	}

	if (HealingParametersComp && (Effect->DurationPolicy != EGameplayEffectDurationType::Instant || Effect->Executions.Num() != 1
		|| Effect->Executions[0].CalculationClass != UCombatHealingExecution::StaticClass() || !Effect->Executions[0].CalculationModifiers.IsEmpty()
		|| !Effect->Executions[0].ConditionalGameplayEffects.IsEmpty() || !Effect->GetGrantedTags().IsEmpty()))
	{
		return false;
	}

	if (ShieldEffectComp)
	{
		float Duration = 0;
		if (Effect->DurationPolicy != EGameplayEffectDurationType::HasDuration || !Effect->Executions.IsEmpty()
			|| Effect->DurationMagnitude.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::ScalableFloat
			|| !Effect->DurationMagnitude.GetStaticMagnitudeIfPossible(Level, Duration) || !FMath::IsFinite(Duration) || Duration <= 0
			|| !Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_Shield) || !Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_RemoveOnDeath))
		{
			return false;
		}
	}

	TArray<UClass*> EffectComponentClasses;
	GetDerivedClasses(UGameplayEffectComponent::StaticClass(), EffectComponentClasses, true);
	for (UClass* EffectComponentClass : EffectComponentClasses)
	{
		const auto* Component = Effect->FindComponent(EffectComponentClass);
		if (Component && Component->GetClass() != UCombatHealingParametersComponent::StaticClass() && Component->GetClass() != UCombatShieldEffectComponent::StaticClass()
			&& Component->GetClass() != UTargetTagsGameplayEffectComponent::StaticClass() && Component->GetClass() != UAssetTagsGameplayEffectComponent::StaticClass())
		{
			return false;
		}
	}

	return true;
}

