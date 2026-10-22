// ShooterAI.Build.cs

using UnrealBuildTool;

public class ShooterAI : ModuleRules
{
	public ShooterAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"NavigationSystem",
			"AIModule",
			"StateTreeModule",
			// UStateTreeComponent 在这个插件的模块里（不是 StateTreeModule）。
			// LOD 要调它的 SetComponentTickInterval，所以必须显式依赖。
			"GameplayStateTreeModule"
		});
	}
}
