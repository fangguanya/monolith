using UnrealBuildTool;
using System.IO;

public class MonolithMass : ModuleRules
{
	public MonolithMass(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Core Monolith infrastructure — always required.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore", "MonolithBlueprint", "MonolithIndex",
			"UnrealEd",
			"Json", "JsonUtilities"
		});

		// Matrix projects bundle CitySample / MassTraffic / MassCrowd by default, but the Monolith
		// plugin itself is designed to be drop-in for projects that may not have them.
		// Mirror the conditional probe pattern used by MonolithAI to keep the host plugin portable.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		string EngineDir = "";
		string EnginePluginsDir = "";
		string ProjectPluginsDir = "";
		if (!bReleaseBuild)
		{
			EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
			if (Target.ProjectFile != null)
			{
				string ProjectDir = Path.GetDirectoryName(Target.ProjectFile.FullName);
				ProjectPluginsDir = Path.Combine(ProjectDir, "Plugins");
			}
		}

		// Helper — check both Engine/Plugins and <Project>/Plugins recursively (1 level each).
		bool bHasZoneGraph = false;
		bool bHasMassEntity = false;
		bool bHasMassCrowd = false;
		bool bHasMassTraffic = false;

		if (!bReleaseBuild)
		{
			string[] SearchDirs = new string[]
			{
				EnginePluginsDir,
				Path.Combine(EnginePluginsDir, "Runtime"),
				Path.Combine(EnginePluginsDir, "Experimental"),
				Path.Combine(EnginePluginsDir, "AI"),
				ProjectPluginsDir,
				Path.Combine(ProjectPluginsDir, "Matrix"),
				Path.Combine(ProjectPluginsDir, "CitySample"),
				ProjectPluginsDir  // direct project-level plugins (e.g. Client/Plugins/Traffic)
			};

			foreach (string Dir in SearchDirs)
			{
				if (string.IsNullOrEmpty(Dir) || !Directory.Exists(Dir)) continue;

				if (!bHasZoneGraph &&
					Directory.GetDirectories(Dir, "ZoneGraph*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasZoneGraph = true;
				}
				if (!bHasMassEntity &&
					Directory.GetDirectories(Dir, "MassEntity*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasMassEntity = true;
				}
				if (!bHasMassCrowd &&
					Directory.GetDirectories(Dir, "MassCrowd*", SearchOption.TopDirectoryOnly).Length > 0)
				{
					bHasMassCrowd = true;
				}
				// MassTraffic is the CitySample plugin — lives under Client/Plugins/Traffic in this project.
				if (!bHasMassTraffic &&
					Directory.GetDirectories(Dir, "Traffic", SearchOption.TopDirectoryOnly).Length > 0 &&
					File.Exists(Path.Combine(Dir, "Traffic", "Source", "MassTraffic", "MassTraffic.Build.cs")))
				{
					bHasMassTraffic = true;
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

		if (bHasMassEntity)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MassEntity", "MassCommon", "MassSpawner"
			});
			PublicDefinitions.Add("WITH_MASSENTITY=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MASSENTITY=0");
		}

		if (bHasMassCrowd)
		{
			PrivateDependencyModuleNames.Add("MassCrowd");
			PublicDefinitions.Add("WITH_MASSCROWD=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MASSCROWD=0");
		}

		if (bHasMassTraffic)
		{
			PrivateDependencyModuleNames.Add("MassTraffic");
			PublicDefinitions.Add("WITH_MASSTRAFFIC=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MASSTRAFFIC=0");
		}
	}
}
