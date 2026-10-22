#include "AbilitySystem/SelfCommitAbility.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroAttributeComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatCooldown.h"
#include "MobaLogChannels.h"
#include "MobaGameplayTags.h"
#include "AbilitySystemComponent.h"

USelfCommitAbility::USelfCommitAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	// 本例没有需预测的表现；服务器从验证后的稳定槽入口激活。仍用原生 GAS 成本/冷却。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
	FGameplayTagContainer Tags(MobaGameplayTags::Ability_CancelOnDeath);
	Tags.AddTag(MobaGameplayTags::Ability_Active);
	Tags.AddTag(MobaGameplayTags::Ability_Spell);
	SetAssetTags(Tags);
}

bool USelfCommitAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	const AHeroCharacter* Hero = ActorInfo ? Cast<AHeroCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	const auto* ASC = ActorInfo ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
	if (!ASC || ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun)
		|| (GetAssetTags().HasTagExact(MobaGameplayTags::Ability_Spell) && ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Silence)))
	{
		return false;
	}

	if (Hero && Hero->HasAuthority())
	{
		Hero->GetAttributeComponent()->FlushRegeneration();
	}

	return Hero && Hero->CanAcceptPlayerInput() && Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

bool USelfCommitAbility::CheckCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (const auto* Effect = GetCooldownGameplayEffect(); Effect && Effect->FindComponent<UAbilityCooldownParametersComponent>())
	{
		const auto* ASC = ActorInfo ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
		if (!ASC)
		{
			return false;
		}

		const FGameplayEffectSpec Spec(Effect, ASC->MakeEffectContext(), GetAbilityLevel(Handle, ActorInfo));
		if (!FMath::IsFinite(Spec.GetDuration()) || Spec.GetDuration() <= 0)
		{
			return false;
		}
	}

	return Super::CheckCooldown(Handle, ActorInfo, OptionalRelevantTags);
}

bool USelfCommitAbility::CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags) const
{
	const auto* Hero = ActorInfo ? Cast<AHeroCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (Hero && Hero->HasAuthority())
	{
		auto* Component = Hero->GetAttributeComponent();
		if (Hero->GetAttributes()->IsSettlementBusy())
		{
			return false;
		}

		Component->FlushRegeneration();
		if (const auto* CostEffect = GetCostGameplayEffect())
		{
			if (CostEffect->DurationPolicy != EGameplayEffectDurationType::Instant || !CostEffect->Executions.IsEmpty()
				|| CostEffect->Period.GetValueAtLevel(GetAbilityLevel(Handle, ActorInfo)) != 0
				|| CostEffect->Modifiers.ContainsByPredicate([](const FGameplayModifierInfo& Modifier) { return Modifier.Attribute != UHeroAttributeSet::GetManaAttribute(); }))
			{
				return false;
			}

			const FGameplayEffectSpec CostSpec(CostEffect, ActorInfo->AbilitySystemComponent->MakeEffectContext(), GetAbilityLevel(Handle, ActorInfo));
			if (!Component->ValidateModifiers(CostSpec))
			{
				return false;
			}
		}
	}

	return Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags);
}

void USelfCommitAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	const AHeroCharacter* Hero = CastChecked<AHeroCharacter>(ActorInfo->AvatarActor.Get());
	UE_LOG(LogMobaControl, Display, TEXT("SelfAbility Entity=%s Committed=%d Mana=%.2f"), *Hero->EntityId.ToString(), bCommitted, Hero->GetAttributes()->GetMana());
	EndAbility(Handle, ActorInfo, ActivationInfo, true, !bCommitted);
}
