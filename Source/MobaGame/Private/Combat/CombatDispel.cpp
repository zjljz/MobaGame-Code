#include "Combat/CombatDispel.h"

#include "AbilitySystemComponent.h"
#include "MobaGameplayTags.h"

int32 MobaCombat::Dispel(UAbilitySystemComponent* TargetASC, bool bStrong, bool bBeneficial, bool bHarmful)
{
	if (!TargetASC || !TargetASC->GetOwner() || !TargetASC->GetOwner()->HasAuthority())
	{
		return 0;
	}

	int32 RemovedEffectCount = 0;
	for (const auto EffectHandle : TargetASC->GetActiveEffects(FGameplayEffectQuery()))
	{
		const auto* ActiveEffect = TargetASC->GetActiveGameplayEffect(EffectHandle);
		if (!ActiveEffect)
		{
			continue;
		}

		FGameplayTagContainer Tags;
		ActiveEffect->Spec.GetAllGrantedTags(Tags);
		if (Tags.HasTagExact(MobaGameplayTags::Effect_Dispel_Undispellable) || Tags.HasTag(MobaGameplayTags::Cooldown_Self.GetTag().RequestDirectParent()))
		{
			continue;
		}

		if (!(bBeneficial && Tags.HasTagExact(MobaGameplayTags::Effect_Beneficial)) && !(bHarmful && Tags.HasTagExact(MobaGameplayTags::Effect_Harmful)))
		{
			continue;
		}

		if (Tags.HasTagExact(MobaGameplayTags::Effect_Dispel_Basic) || (bStrong && Tags.HasTagExact(MobaGameplayTags::Effect_Dispel_Strong)))
		{
			RemovedEffectCount += TargetASC->RemoveActiveGameplayEffect(EffectHandle) ? 1 : 0;
		}
	}

	return RemovedEffectCount;
}

