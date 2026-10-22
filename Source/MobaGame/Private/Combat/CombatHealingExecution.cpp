#include "Combat/CombatHealingExecution.h"

#include "Combat/CombatEffectContext.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/CombatantInterface.h"
#include "AbilitySystemComponent.h"

UCombatHealingExecution::UCombatHealingExecution()
{
	RelevantAttributesToCapture = {MobaCombat::SourceAttackDamageCapture(), MobaCombat::SourceAbilityPowerCapture()};
}

void UCombatHealingExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const
{
	const FGameplayEffectSpec& Spec = Parameters.GetOwningSpec();
	auto* Context = MobaCombat::GetContext(Spec.GetContext());
	if (Context)
	{
		Context->Result = {};
	}

	const auto* ASC = Parameters.GetTargetAbilitySystemComponent();
	const auto* Actor = ASC ? ASC->GetAvatarActor() : nullptr;
	const auto* TargetCombatant = Cast<ICombatantInterface>(Actor);
	const auto* HealingComp = Spec.Def->FindComponent<UCombatHealingParametersComponent>();
	float Amount = 0;
	if (!TargetCombatant || !Actor->HasAuthority() || !TargetCombatant->IsCombatAlive() || !HealingComp
		|| !MobaCombat::ResolveRawAmount(Spec, HealingComp->HealAmount, HealingComp->AttackDamageCoefficient, HealingComp->AbilityPowerCoefficient, Amount))
	{
		return;
	}

	const double Stacked = double(Amount) * Spec.GetStackCount();
	if (!FMath::IsFinite(Stacked) || Stacked > MAX_flt)
	{
		return;
	}

	Output.MarkStackCountHandledManually();
	if (Context)
	{
		Context->Result.Amount = float(Stacked);
	}

	Output.AddOutputModifier(FGameplayModifierEvaluatedData(UHeroAttributeSet::GetIncomingHealAttribute(), EGameplayModOp::Additive, float(Stacked)));
}
