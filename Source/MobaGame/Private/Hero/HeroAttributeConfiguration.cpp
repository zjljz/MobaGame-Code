#include "Hero/HeroAttributeConfiguration.h"

bool FHeroAttributeRules::IsValid(FString& Reason) const
{
	if (ExperienceThresholds.IsEmpty() || ExperienceThresholds[0] != 0 || ExperienceThresholds.Num() > 16777216
		|| !FMath::IsFinite(MaximumCooldownReduction) || MaximumCooldownReduction < 0 || MaximumCooldownReduction >= 1
		|| !FMath::IsFinite(MinimumSlowMultiplier) || MinimumSlowMultiplier <= 0 || MinimumSlowMultiplier > 1)
	{
		Reason = TEXT("AttributeRules");
		return false;
	}

	for (int32 Index = 1; Index < ExperienceThresholds.Num(); ++Index)
	{
		if (ExperienceThresholds[Index] <= ExperienceThresholds[Index - 1])
		{
			Reason = TEXT("ExperienceThresholds");
			return false;
		}
	}

	return true;
}
