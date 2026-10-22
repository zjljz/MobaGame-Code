#include "Combat/CombatDamageExecution.h"

#include "Combat/CombatEffectContext.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatantInterface.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Engine/World.h"
#include "AbilitySystemComponent.h"
#include "MobaGameplayTags.h"

UCombatDamageExecution::UCombatDamageExecution()
{
	RelevantAttributesToCapture = {
		MobaCombat::SourceAttackDamageCapture(), MobaCombat::SourceAbilityPowerCapture(),
		{UHeroAttributeSet::GetCriticalChanceAttribute(), EGameplayEffectAttributeCaptureSource::Source, true},
		{UHeroAttributeSet::GetCriticalDamageMultiplierAttribute(), EGameplayEffectAttributeCaptureSource::Source, true},
		{UHeroAttributeSet::GetArmorAttribute(), EGameplayEffectAttributeCaptureSource::Target, false},
		{UHeroAttributeSet::GetMagicResistanceAttribute(), EGameplayEffectAttributeCaptureSource::Target, false},
		{UHeroAttributeSet::GetDamageTakenMultiplierAttribute(), EGameplayEffectAttributeCaptureSource::Target, false}};
}

void UCombatDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const
{
	const FGameplayEffectSpec& Spec = Parameters.GetOwningSpec();
	auto* Context = MobaCombat::GetContext(Spec.GetContext());
	if (Context)
	{
		Context->Result = {};
	}

	const auto* TargetASC = Parameters.GetTargetAbilitySystemComponent();
	const auto* TargetActor = TargetASC ? TargetASC->GetAvatarActor() : nullptr;
	const auto* TargetCombatant = Cast<ICombatantInterface>(TargetActor);
	if (!TargetCombatant || !TargetActor->HasAuthority() || !TargetCombatant->IsCombatAlive()
		|| TargetASC->HasMatchingGameplayTag(MobaGameplayTags::State_Invulnerable))
	{
		if (Context)
		{
			Context->Result.Rejection = TEXT("TargetUnavailable");
		}

		return;
	}

	if (Context && Context->Match.IsValid() && Context->Match != TargetCombatant->GetCombatMatch())
	{
		Context->Result.Rejection = TEXT("Match");
		return;
	}

	const auto* Rules = TargetActor->GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	const auto* DamageComp = Spec.Def->FindComponent<UCombatDamageParametersComponent>();
	FGameplayTagContainer Tags;
	Spec.GetAllAssetTags(Tags);
	ECombatDamageType Type;
	FCombatDamageParameters Values;
	if (!Rules || !DamageComp || !Rules->CombatRules.ResolveDamageType(Tags, Type) || !DamageComp->Resolve(Spec, Values))
	{
		return;
	}

	// 周期一律不暴击；0/100% 不消耗随机样本。聚合捕获不假定经过 CurrentValue clamp。
	Values.bCanCritical = (Values.bCanCritical && Spec.GetPeriod() == 0);
	Values.CriticalChance = 0;
	Values.CriticalMultiplier = 1;
	if (Values.bCanCritical)
	{
		if (!MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[2], Values.CriticalChance)
			|| !MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[3], Values.CriticalMultiplier))
		{
			return;
		}

		Values.CriticalChance = FMath::Clamp(Values.CriticalChance, 0.0f, 1.0f);
		Values.CriticalMultiplier = FMath::Max(1.0f, Values.CriticalMultiplier);
		Values.CriticalSample = (Values.CriticalChance > 0 && Values.CriticalChance < 1) ? FMath::FRand() : 1;
	}

	if (!MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[4], Values.Armor)
		|| !MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[5], Values.MagicResistance)
		|| !MobaCombat::EvaluateCapture(Spec, RelevantAttributesToCapture[6], Values.DamageTakenMultiplier))
	{
		return;
	}

	Values.DamageTakenMultiplier = FMath::Max(0.0f, Values.DamageTakenMultiplier);
	const double Stacked = double(Values.BaseDamage) * Spec.GetStackCount();
	if (!FMath::IsFinite(Stacked) || Stacked > MAX_flt)
	{
		return;
	}

	Values.BaseDamage = float(Stacked);
	float Damage = 0;
	bool bCritical = false;
	if (!MobaCombat::CalculateDamage(Type, Values, Damage, bCritical))
	{
		return;
	}

	Output.MarkStackCountHandledManually();
	if (Context)
	{
		Context->Result.Amount = Damage;
		Context->Result.bCritical = bCritical;
	}

	Output.AddOutputModifier(FGameplayModifierEvaluatedData(UHeroAttributeSet::GetIncomingDamageAttribute(), EGameplayModOp::Additive, Damage));
}
