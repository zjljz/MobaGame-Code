#include "Hero/HeroDefinition.h"

#include "AbilitySystem/AbilitySetDefinition.h"
#include "Engine/StaticMesh.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#if WITH_EDITOR
EDataValidationResult UHeroDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	AbilitySet.LoadSynchronous();
	FString Reason;
	if (!ValidateInitialization(Reason))
	{
		Context.AddError(FText::FromString(Reason));
	}

	// 单资产校验一级边界；所有可达等级由规则资产及服务器初始化联合校验。
	FHeroAttributeRules Rules;
	Rules.ExperienceThresholds = {0};
	Rules.MinimumSlowMultiplier = 1;
	if (!ValidateAttributes(Rules, Reason) || !BodyMesh.LoadSynchronous())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "HeroAttributes", "Hero 需要合法一级属性与形体；成长还需结合比赛门槛校验。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UHeroDefinition::ValidateInitialization(FString& Reason) const
{
	if (!AbilitySet.IsValid() || !FMath::IsFinite(MaxClickDistance) || MaxClickDistance <= 0 || !FMath::IsFinite(RespawnDelay) || RespawnDelay <= 0)
	{
		Reason = TEXT("HeroInitializationConfiguration");
		return false;
	}

	return true;
}

bool UHeroDefinition::ValidateAttributes(const FHeroAttributeRules& Rules, FString& Reason) const
{
	if (!Rules.IsValid(Reason) || ResourceType > EHeroResourceType::None)
	{
		return false;
	}

	const int32 LastLevel = Rules.ExperienceThresholds.Num();
	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.MaxHealth.BaseValue) + double(Attributes.MaxHealth.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 1)
		{
			Reason = TEXT("HeroAttribute:MaxHealth");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.MaxMana.BaseValue) + double(Attributes.MaxMana.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || (ResourceType == EHeroResourceType::None ? Value != 0 : Value < 1))
		{
			Reason = TEXT("HeroAttribute:MaxMana");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.HealthRegeneration.BaseValue) + double(Attributes.HealthRegeneration.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0)
		{
			Reason = TEXT("HeroAttribute:HealthRegeneration");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.ManaRegeneration.BaseValue) + double(Attributes.ManaRegeneration.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || (ResourceType == EHeroResourceType::None ? Value != 0 : Value < 0))
		{
			Reason = TEXT("HeroAttribute:ManaRegeneration");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.AttackDamage.BaseValue) + double(Attributes.AttackDamage.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0)
		{
			Reason = TEXT("HeroAttribute:AttackDamage");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.AbilityPower.BaseValue) + double(Attributes.AbilityPower.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0)
		{
			Reason = TEXT("HeroAttribute:AbilityPower");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.Armor.BaseValue) + double(Attributes.Armor.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
		{
			Reason = TEXT("HeroAttribute:Armor");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.MagicResistance.BaseValue) + double(Attributes.MagicResistance.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
		{
			Reason = TEXT("HeroAttribute:MagicResistance");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.CriticalChance.BaseValue) + double(Attributes.CriticalChance.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0 || Value > 1)
		{
			Reason = TEXT("HeroAttribute:CriticalChance");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.CriticalDamageMultiplier.BaseValue) + double(Attributes.CriticalDamageMultiplier.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 1)
		{
			Reason = TEXT("HeroAttribute:CriticalDamageMultiplier");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.CooldownReduction.BaseValue) + double(Attributes.CooldownReduction.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0 || Value > Rules.MaximumCooldownReduction)
		{
			Reason = TEXT("HeroAttribute:CooldownReduction");
			return false;
		}
	}

	for (const int32 Level : {1, LastLevel})
	{
		const double Value = double(Attributes.MoveSpeed.BaseValue) + double(Attributes.MoveSpeed.GrowthPerLevel) * (Level - 1);
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt || Value < 0 || Value <= 0)
		{
			Reason = TEXT("HeroAttribute:MoveSpeed");
			return false;
		}
	}

	return true;
}
