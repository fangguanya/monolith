using UnrealBuildTool;

public class MonolithCapture : ModuleRules
{
	public MonolithCapture(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"RenderCore",
			"RHI",
			"ImageWrapper",
			"ImageCore",
			"Niagara",
			"AdvancedPreviewScene",
			"UMG",
			"UMGEditor",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"EditorScriptingUtilities",
			"AnimationCore"
		});
	}
}
