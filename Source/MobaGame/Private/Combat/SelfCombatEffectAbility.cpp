#include "Combat/SelfCombatEffectAbility.h"

#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/CombatEffectContext.h"
#include "AbilitySystemComponent.h"

bool USelfCombatEffectAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Effect || !Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}

	const auto* ASC = ActorInfo->AbilitySystemComponent.Get();
	const auto* SelfEffectCDO = Effect.GetDefaultObject();
	bool bHealing = false;
	float Amount = 0;
	if (!MobaCombat::ValidateSelfEffect(SelfEffectCDO, GetAbilityLevel(Handle, ActorInfo), bHealing, Amount)
		|| (bHealing && ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()) >= ASC->GetNumericAttribute(UHeroAttributeSet::GetMaxHealthAttribute())))
	{
		return false;
	}

	// Commit会改变资源和冷却标签，未知查询可能因此从允许翻转为拒绝，不能承诺其可回滚。
	if (!ASC->GameplayEffectApplicationQueries.IsEmpty())
	{
		return false;
	}

	const auto Prepared = MobaCombat::PrepareSpec(const_cast<UAbilitySystemComponent*>(ASC), SelfEffectCDO, GetAbilityLevel(Handle, ActorInfo));
	const auto* HealingComp = SelfEffectCDO->FindComponent<UCombatHealingParametersComponent>();
	const auto* ShieldComp = SelfEffectCDO->FindComponent<UCombatShieldEffectComponent>();
	if (!Prepared.IsValid() || !MobaCombat::ResolveRawAmount(*Prepared.Data, HealingComp ? HealingComp->HealAmount : ShieldComp->ShieldAmount,
		HealingComp ? HealingComp->AttackDamageCoefficient : ShieldComp->AttackDamageCoefficient,
		HealingComp ? HealingComp->AbilityPowerCoefficient : ShieldComp->AbilityPowerCoefficient, Amount)
		|| !SelfEffectCDO->CanApply(ASC->GetActiveGameplayEffects(), *Prepared.Data))
	{
		return false;
	}

	return true;
}

void USelfCombatEffectAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* Event)
{
	bEffectCommitted = false;
	if (CanActivateAbility(Handle, ActorInfo))
	{
		auto* ASC = ActorInfo->AbilitySystemComponent.Get();
		const auto Prepared = MobaCombat::PrepareSpec(ASC, Effect, GetAbilityLevel(Handle, ActorInfo));
		if (Prepared.IsValid() && CommitAbility(Handle, ActorInfo, ActivationInfo))
		{
			const auto AppliedHandle = ASC->ApplyGameplayEffectSpecToSelf(*Prepared.Data);
			const auto* Context = MobaCombat::GetContext(Prepared.Data->GetContext());
			bEffectCommitted = ((Context && Context->Result.bApplied) || (Effect.GetDefaultObject()->DurationPolicy != EGameplayEffectDurationType::Instant && AppliedHandle.IsValid()));
		}
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, !bEffectCommitted);
}

