using UnrealBuildTool;

public class MonolithBlueprint : ModuleRules
{
	public MonolithBlueprint(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
			"BlueprintGraph",
			"AnimGraph",        // Wave 11: AnimationGraph schema + AnimGraphNode_Root/LinkedInputPose for ALI functions
			"BlueprintEditorLibrary",
			"SubobjectDataInterface",
			"Kismet",
			"KismetCompiler",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities"
		});
	}
}
