// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MobaGame : ModuleRules
{
	public MobaGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core", "CoreUObject", "Engine", "GameFeatures", "GMP", "GameplayAbilities", "GameplayTags", "TargetingSystem"
		});
		
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry", "Projects", "GameplayTasks", "EnhancedInput", "InputCore", "AIModule", "NavigationSystem", "NetCore"
		});
		
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new[] { "bcrypt.lib", "crypt32.lib" });
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}
	}
}
