#include "Combat/CombatRules.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Match/MatchGameState.h"
#include "Engine/World.h"
#include "MobaGameplayTags.h"

namespace
{
FGameplayTag GetDamageRoot(ECombatDamageType Type)
{
	switch (Type)
	{
	case ECombatDamageType::Physical: return MobaGameplayTags::Damage_Physical;
	case ECombatDamageType::Magical: return MobaGameplayTags::Damage_Magical;
	case ECombatDamageType::TrueDamage: return MobaGameplayTags::Damage_True;
	default: return FGameplayTag();
	}
}

bool IsValidDamageMapping(FGameplayTag Tag, ECombatDamageType Type)
{
	const FGameplayTag Root = GetDamageRoot(Type);
	return Root.IsValid() && Tag != Root && Tag.MatchesTag(Root);
}

/**
 * 检查整组伤害参数，拒绝 NaN、无穷及越界值；本次未使用的抗性或暴击参数也必须有效。
 */
bool AreDamageParametersValid(const FCombatDamageParameters& Parameters)
{
	return FMath::IsFinite(Parameters.BaseDamage) && Parameters.BaseDamage > 0 && FMath::IsFinite(Parameters.Armor) && FMath::IsFinite(Parameters.MagicResistance)
		&& FMath::IsFinite(Parameters.DamageTakenMultiplier) && Parameters.DamageTakenMultiplier >= 0
		&& FMath::IsFinite(Parameters.CriticalChance) && Parameters.CriticalChance >= 0 && Parameters.CriticalChance <= 1
		&& FMath::IsFinite(Parameters.CriticalMultiplier) && Parameters.CriticalMultiplier >= 1
		&& FMath::IsFinite(Parameters.CriticalSample) && Parameters.CriticalSample >= 0 && Parameters.CriticalSample <= 1;
}
}

bool FCombatRules::ResolveDamageType(const FGameplayTagContainer& Tags, ECombatDamageType& OutType) const
{
	OutType = ECombatDamageType::TrueDamage;
	FGameplayTag SelectedDamageTag;
	for (const FGameplayTag Tag : Tags)
	{
		if (!Tag.MatchesTag(MobaGameplayTags::Damage))
		{
			continue;
		}

		if (SelectedDamageTag.IsValid())
		{
			return false;
		}

		SelectedDamageTag = Tag;
	}

	for (const ECombatDamageType Type : {ECombatDamageType::Physical, ECombatDamageType::Magical, ECombatDamageType::TrueDamage})
	{
		if (SelectedDamageTag == GetDamageRoot(Type))
		{
			OutType = Type;
			return true;
		}
	}

	const ECombatDamageType* MappedDamageType = DamageTypeMappings.Find(SelectedDamageTag);
	if (!MappedDamageType || !IsValidDamageMapping(SelectedDamageTag, *MappedDamageType))
	{
		return false;
	}

	OutType = *MappedDamageType;
	return true;
}

bool FCombatRules::AllowsRelation(ETeamRelation Relation) const
{
	return Relation == ETeamRelation::Enemy || (Relation == ETeamRelation::Neutral && bAllowNeutralTargets);
}

bool FCombatRules::IsValid() const
{
	for (const auto& DamageTypeMapping : DamageTypeMappings)
	{
		if (!IsValidDamageMapping(DamageTypeMapping.Key, DamageTypeMapping.Value))
		{
			return false;
		}
	}

	return true;
}

bool MobaCombat::CalculateDamage(ECombatDamageType Type, const FCombatDamageParameters& Parameters, float& OutDamage, bool& bOutCritical)
{
	OutDamage = 0;
	bOutCritical = false;
	if (!AreDamageParametersValid(Parameters))
	{
		return false;
	}

	const bool bCritical = Parameters.bCanCritical && (Parameters.CriticalChance == 1 || Parameters.CriticalSample < Parameters.CriticalChance);
	const double Resistance = Type == ECombatDamageType::Physical ? Parameters.Armor : Parameters.MagicResistance;
	const double ResistanceMultiplier = Type == ECombatDamageType::TrueDamage ? 1.0 : (Resistance >= 0 ? 100.0 / (100.0 + Resistance) : 2.0 - 100.0 / (100.0 - Resistance));
	// 使用 double 中间值，避免有限 float 抗性在分母求和时溢出。
	const double Damage = static_cast<double>(Parameters.BaseDamage) * (bCritical ? Parameters.CriticalMultiplier : 1.0) * ResistanceMultiplier * Parameters.DamageTakenMultiplier;
	if (!FMath::IsFinite(Damage) || Damage > MAX_flt)
	{
		return false;
	}

	OutDamage = static_cast<float>(Damage);
	bOutCritical = bCritical;
	return true;
}

