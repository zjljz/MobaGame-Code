using UnrealBuildTool;

public class MobaGameTests : ModuleRules
{
	public MobaGameTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "MobaGame", "GameplayAbilities", "GameplayTags",
			"GameFeatures", "GMP", "EnhancedInput", "InputCore", "AIModule", "NavigationSystem", "NetCore", "AssetRegistry"
		});
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new[] { "DataValidation", "UnrealEd" });
		}
	}
}
