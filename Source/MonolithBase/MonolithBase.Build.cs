using UnrealBuildTool;

public class MonolithBase : ModuleRules
{
	public MonolithBase(ReadOnlyTargetRules Target) : base(Target)
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
			"Json",
			"JsonUtilities",
			"EditorScriptingUtilities",
			"LevelEditor",
			"Slate",
			"SlateCore",
			"RenderCore"
		});
	}
}
