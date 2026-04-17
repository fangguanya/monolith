using UnrealBuildTool;
using System.IO;

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
			"RenderCore",
			"NavigationSystem"  // bake.navmesh
		});

		// ZoneGraph is required by `bake.zone_graph`. Detect it the same way MonolithMass does so
		// the host plugin keeps the same portability story.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasZoneGraph = false;
		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
			string[] SearchDirs = new string[]
			{
				EnginePluginsDir,
				Path.Combine(EnginePluginsDir, "Runtime"),
				Path.Combine(EnginePluginsDir, "Experimental")
			};
			foreach (string Dir in SearchDirs)
			{
				if (Directory.Exists(Dir) &&
					Directory.GetDirectories(Dir, "ZoneGraph*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasZoneGraph = true;
					break;
				}
			}
		}
		if (bHasZoneGraph)
		{
			PrivateDependencyModuleNames.Add("ZoneGraph");
			PublicDefinitions.Add("WITH_ZONEGRAPH=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_ZONEGRAPH=0");
		}
	}
}
