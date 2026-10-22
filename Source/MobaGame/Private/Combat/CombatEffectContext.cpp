#include "Combat/CombatEffectContext.h"

#include "AbilitySystemComponent.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/CombatantInterface.h"
#include "Combat/HeroAttributeSet.h"
#include "GameplayEffectAggregator.h"

UScriptStruct* FCombatEffectContext::GetScriptStruct() const
{
	return StaticStruct();
}

FGameplayEffectContext* FCombatEffectContext::Duplicate() const
{
	auto* Copy = new FCombatEffectContext(*this);
	if (GetHitResult())
	{
		Copy->AddHitResult(*GetHitResult(), true);
	}

	Copy->Result = {};
	return Copy;
}

bool FCombatEffectContext::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	bool bBaseSuccess = false;
	FGameplayEffectContext::NetSerialize(Ar, Map, bBaseSuccess);
	Ar << Match << Participant << Entity << Delivery << Team;
	if (Ar.IsLoading())
	{
		Result = {};
	}

	bOutSuccess = (bBaseSuccess && !Ar.IsError());
	return bOutSuccess;
}

FCombatEffectContext* MobaCombat::GetContext(const FGameplayEffectContextHandle& Context)
{
	return Context.IsValid() && Context.Get()->GetScriptStruct()->IsChildOf(FCombatEffectContext::StaticStruct())
		? const_cast<FCombatEffectContext*>(static_cast<const FCombatEffectContext*>(Context.Get())) : nullptr;
}

FGameplayEffectSpecHandle MobaCombat::PrepareSpec(UAbilitySystemComponent* Source, const UGameplayEffect* Effect, float Level, FGuid Delivery)
{
	if (!Source || !Effect || !FMath::IsFinite(Level) || Level <= 0)
	{
		return {};
	}

	auto* Context = new FCombatEffectContext();
	static_cast<FGameplayEffectContext&>(*Context) = *Source->MakeEffectContext().Get();
	if (const auto* Combatant = Cast<ICombatantInterface>(Source->GetAvatarActor()))
	{
		Context->Match = Combatant->GetCombatMatch();
		Context->Entity = Combatant->GetCombatEntity();
		Context->Participant = Combatant->GetCombatParticipant();
		Context->Team = Combatant->GetCombatTeam();
	}

	Context->Delivery = Delivery.IsValid() ? Delivery : FGuid::NewGuid();
	FGameplayEffectSpecHandle Spec(new FGameplayEffectSpec(Effect, FGameplayEffectContextHandle(Context), Level));
	// GE Component 字段不会被引擎自动扫描；护盾必须显式注册并捕获来源。
	if (Effect->FindComponent<UCombatShieldEffectComponent>())
	{
		Spec.Data->CapturedRelevantAttributes.AddCaptureDefinition(SourceAttackDamageCapture());
		Spec.Data->CapturedRelevantAttributes.AddCaptureDefinition(SourceAbilityPowerCapture());
		Spec.Data->CapturedRelevantAttributes.CaptureAttributes(Source, EGameplayEffectAttributeCaptureSource::Source);
	}

	return Spec;
}

FGameplayEffectSpecHandle MobaCombat::PrepareSpec(UAbilitySystemComponent* Source, TSubclassOf<UGameplayEffect> Effect, float Level, FGuid Delivery)
{
	return PrepareSpec(Source, Effect.GetDefaultObject(), Level, Delivery);
}

FGameplayEffectSpec MobaCombat::CopyForTarget(const FGameplayEffectSpec& Spec)
{
	FGameplayEffectSpec Copy(Spec);
	// SetContext 会重新捕获来源；原生专用复制接口只换独立 Context。
	Copy.DuplicateEffectContext();
	return Copy;
}

FGameplayEffectAttributeCaptureDefinition MobaCombat::SourceAttackDamageCapture()
{
	return {UHeroAttributeSet::GetAttackDamageAttribute(), EGameplayEffectAttributeCaptureSource::Source, true};
}

FGameplayEffectAttributeCaptureDefinition MobaCombat::SourceAbilityPowerCapture()
{
	return {UHeroAttributeSet::GetAbilityPowerAttribute(), EGameplayEffectAttributeCaptureSource::Source, true};
}

bool MobaCombat::ResolveRawAmount(const FGameplayEffectSpec& Spec, const FGameplayEffectModifierMagnitude& Base,
	const FScalableFloat& AttackCoefficient, const FScalableFloat& AbilityCoefficient, float& Amount)
{
	Amount = 0;
	float BaseAmount = 0;
	const float AttackScale = AttackCoefficient.GetValueAtLevel(Spec.GetLevel());
	const float AbilityScale = AbilityCoefficient.GetValueAtLevel(Spec.GetLevel());
	float Attack = 0;
	float Power = 0;
	if (!ResolveMagnitude(Base, Spec, BaseAmount) || BaseAmount < 0 || !FMath::IsFinite(AttackScale) || AttackScale < 0
		|| !FMath::IsFinite(AbilityScale) || AbilityScale < 0
		|| (AttackScale > 0 && !EvaluateCapture(Spec, SourceAttackDamageCapture(), Attack))
		|| (AbilityScale > 0 && !EvaluateCapture(Spec, SourceAbilityPowerCapture(), Power)))
	{
		return false;
	}

	const double Raw = double(BaseAmount) + double(AttackScale) * FMath::Max(0.0f, Attack) + double(AbilityScale) * FMath::Max(0.0f, Power);
	if (!FMath::IsFinite(Raw) || Raw <= 0 || Raw > MAX_flt)
	{
		return false;
	}

	Amount = float(Raw);
	return true;
}

bool MobaCombat::EvaluateCapture(const FGameplayEffectSpec& Spec, const FGameplayEffectAttributeCaptureDefinition& Definition, float& Value)
{
	const auto* Capture = Spec.CapturedRelevantAttributes.FindCaptureSpecByDefinition(Definition, true);
	FAggregatorEvaluateParameters Evaluation;
	Evaluation.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	Evaluation.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();
	return Capture && Capture->AttemptCalculateAttributeMagnitude(Evaluation, Value) && FMath::IsFinite(Value);
}
