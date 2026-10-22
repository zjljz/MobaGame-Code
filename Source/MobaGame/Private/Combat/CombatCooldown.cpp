#include "Combat/CombatCooldown.h"

#include "AbilitySystemComponent.h"
#include "Combat/CombatEffectContext.h"
#include "Combat/HeroAttributeSet.h"
#include "Hero/HeroAttributeComponent.h"
#include "Hero/HeroCharacter.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"

EDataValidationResult UAbilityCooldownParametersComponent::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	const float Duration = BaseDuration.GetValueAtLevel(1);
	const auto& Magnitude = GetOwner()->DurationMagnitude;
	if (!FMath::IsFinite(Duration) || Duration <= 0 || GetOwner()->DurationPolicy != EGameplayEffectDurationType::HasDuration
		|| Magnitude.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::CustomCalculationClass
		|| Magnitude.GetCustomMagnitudeCalculationClass() != UAbilityCooldownMagnitudeCalculation::StaticClass())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "AbilityCooldown", "技能冷却需要正基础时长和共享CDR MMC。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

UAbilityCooldownMagnitudeCalculation::UAbilityCooldownMagnitudeCalculation()
{
	RelevantAttributesToCapture.Add({UHeroAttributeSet::GetCooldownReductionAttribute(), EGameplayEffectAttributeCaptureSource::Source, true});
}

float UAbilityCooldownMagnitudeCalculation::CalculateBaseMagnitude_Implementation(const FGameplayEffectSpec& Spec) const
{
	const auto* Parameters = Spec.Def->FindComponent<UAbilityCooldownParametersComponent>();
	const auto* SourceASC = Spec.GetContext().GetInstigatorAbilitySystemComponent();
	const auto* Hero = SourceASC ? Cast<AHeroCharacter>(SourceASC->GetAvatarActor()) : nullptr;
	float Reduction = 0;
	const float Base = Parameters ? Parameters->BaseDuration.GetValueAtLevel(Spec.GetLevel()) : 0;
	if (!Hero || !Parameters || !FMath::IsFinite(Base) || Base <= 0
		|| !MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[0], Reduction))
	{
		return 0;
	}

	return Base * (1 - FMath::Clamp(Reduction, 0.0f, Hero->GetAttributeComponent()->GetMaximumCooldownReduction()));
}
