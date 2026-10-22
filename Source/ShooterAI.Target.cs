// ShooterAI.Target.cs

using UnrealBuildTool;
using System.Collections.Generic;

public class ShooterAITarget : TargetRules
{
	public ShooterAITarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ShooterAI");
	}
}
