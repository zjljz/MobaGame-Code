#include "AbilitySystem/AbilitySetDefinition.h"

#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ChanceToApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"
#include "UObject/UObjectHash.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#if WITH_EDITOR
EDataValidationResult UAbilitySetDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	for (const FAbilityGrantDefinition& AbilityGrant : Abilities)
	{
		AbilityGrant.Ability.LoadSynchronous();
	}

	for (const auto& EffectClass : Effects)
	{
		EffectClass.LoadSynchronous();
	}

	FString Reason;
	if (!ValidateGrant(Reason))
	{
		Context.AddError(FText::FromString(FString::Printf(TEXT("AbilitySet 配置不满足可撤销授予合同：%s"), *Reason)));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UAbilitySetDefinition::ValidateGrant(FString& Reason) const
{
	if (Abilities.IsEmpty() && Effects.IsEmpty())
	{
		Reason = TEXT("EmptySet");
		return false;
	}

	TSet<FGameplayTag> Slots;
	for (const auto& AbilityGrant : Abilities)
	{
		if (!AbilityGrant.Ability.Get() || AbilityGrant.Level < 1 || (AbilityGrant.MaxLevel < 0 || (AbilityGrant.MaxLevel > 0 && AbilityGrant.MaxLevel < AbilityGrant.Level)))
		{
			Reason = TEXT("AbilityOrLevel");
			return false;
		}

		if (AbilityGrant.Slot.IsValid())
		{
			const FGameplayTag InputSlotRootTag = MobaGameplayTags::Input_Slot_Primary.GetTag().RequestDirectParent();
			if (AbilityGrant.Slot == InputSlotRootTag || !AbilityGrant.Slot.MatchesTag(InputSlotRootTag) || Slots.Contains(AbilityGrant.Slot))
			{
				Reason = TEXT("Slot");
				return false;
			}

			Slots.Add(AbilityGrant.Slot);
		}
	}

	for (const auto& EffectClass : Effects)
	{
		const auto* Effect = EffectClass.Get() ? EffectClass.Get()->GetDefaultObject<UGameplayEffect>() : nullptr;
		if (!Effect || Effect->DurationPolicy == EGameplayEffectDurationType::Instant || Effect->GetStackingType() != EGameplayEffectStackingType::None || !Effect->Executions.IsEmpty()
			|| Effect->Period.GetValueAtLevel(1) != 0)
		{
			Reason = TEXT("EffectPolicy");
			return false;
		}

		if (!HasNonReactiveComponents(Effect))
		{
			Reason = TEXT("EffectComponentPolicy");
			return false;
		}

		// 自定义计算可运行任意代码，不能纳入可撤销来源事务。
		if (Effect->DurationMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::CustomCalculationClass
			|| Effect->MaxDurationMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::CustomCalculationClass)
		{
			Reason = TEXT("EffectCalculationPolicy");
			return false;
		}

		for (const auto& Modifier : Effect->Modifiers)
		{
			if (Modifier.ModifierMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::CustomCalculationClass)
			{
				Reason = TEXT("EffectCalculationPolicy");
				return false;
			}
		}
	}

	return true;
}

bool UAbilitySetDefinition::HasNonReactiveComponents(const UGameplayEffect* Effect)
{
	if (!Effect)
	{
		return false;
	}

	// GE没有公开组件列表接口；通过实际已加载组件类查询其行为对象，拒绝未知/派生回调。
	TArray<UClass*> ComponentClasses;
	GetDerivedClasses(UGameplayEffectComponent::StaticClass(), ComponentClasses, true);
	const TSet<UClass*> SupportedComponentClasses = {UAssetTagsGameplayEffectComponent::StaticClass(), UBlockAbilityTagsGameplayEffectComponent::StaticClass(),
		UChanceToApplyGameplayEffectComponent::StaticClass(), UImmunityGameplayEffectComponent::StaticClass(), UTargetTagRequirementsGameplayEffectComponent::StaticClass(),
		UTargetTagsGameplayEffectComponent::StaticClass()};
	for (UClass* ComponentClass : ComponentClasses)
	{
		const auto* Component = Effect->FindComponent(ComponentClass);
		if (Component && !SupportedComponentClasses.Contains(Component->GetClass()))
		{
			return false;
		}
	}

	const auto* TargetTagRequirementsComp = Effect->FindComponent<UTargetTagRequirementsGameplayEffectComponent>();
	return !TargetTagRequirementsComp || (TargetTagRequirementsComp->RemovalTagRequirements.IsEmpty() && TargetTagRequirementsComp->OngoingTagRequirements.IsEmpty());
}

