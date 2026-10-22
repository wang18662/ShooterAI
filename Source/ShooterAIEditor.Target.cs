// ShooterAIEditor.Target.cs

using UnrealBuildTool;
using System.Collections.Generic;

public class ShooterAIEditorTarget : TargetRules
{
	public ShooterAIEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ShooterAI");
	}
}
