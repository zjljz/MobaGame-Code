using UnrealBuildTool;

public class MobaGameUI : ModuleRules
{
	public MobaGameUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG" });
		PrivateDependencyModuleNames.AddRange(new[] { "MobaGame", "GMP", "GameplayAbilities", "GameplayTags", "Slate", "SlateCore", "InputCore", "EnhancedInput" });
		if (Target.bBuildEditor)
		{
			// 原生离屏渲染 Automation 直接使用 FWidgetRenderer / RenderTarget 读回。
			PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI", "UnrealEd" });
		}
	}
}
