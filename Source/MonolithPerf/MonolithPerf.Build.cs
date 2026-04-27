using UnrealBuildTool;

public class MonolithPerf : ModuleRules
{
	public MonolithPerf(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

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
			"EditorSubsystem",
			"Json",
			"JsonUtilities",
			"RenderCore",
			"RHI",
			"Projects",
			"EngineSettings",
			"HTTP",
			"TraceAnalysis",
			"TraceServices"
		});
	}
}
