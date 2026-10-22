#include "Combat/CombatStateEffectComponent.h"

#include "AbilitySystemComponent.h"
#include "Combat/CombatantInterface.h"
#include "MobaGameplayTags.h"

bool UCombatStateEffectComponent::CanGameplayEffectApply(const FActiveGameplayEffectsContainer& Container, const FGameplayEffectSpec& Spec) const
{
	const auto* Actor = Container.Owner ? Container.Owner->GetAvatarActor() : nullptr;
	const auto* TargetCombatant = Cast<ICombatantInterface>(Actor);
	return TargetCombatant && TargetCombatant->IsCombatAlive();
}

void UCombatStateEffectComponent::OnGameplayEffectApplied(FActiveGameplayEffectsContainer& Container, FGameplayEffectSpec& Spec, FPredictionKey& Key) const
{
	if (!Container.IsNetAuthority())
	{
		return;
	}

	FGameplayTagContainer GrantedTags;
	Spec.GetAllGrantedTags(GrantedTags);
	if (GrantedTags.HasTagExact(MobaGameplayTags::State_Stun))
	{
		FGameplayTagContainer ActiveAbilityTags(MobaGameplayTags::Ability_Active);
		Container.Owner->CancelAbilities(&ActiveAbilityTags);
	}
}

