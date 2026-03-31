using UnrealBuildTool;

public class MonolithLight : ModuleRules
{
	public MonolithLight(ReadOnlyTargetRules Target) : base(Target)
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
			"Landscape",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"Foliage"
		});
	}
}
