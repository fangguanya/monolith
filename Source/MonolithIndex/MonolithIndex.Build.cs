using UnrealBuildTool;
using System.IO;

public class MonolithIndex : ModuleRules
{
	public MonolithIndex(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"Json",
			"JsonUtilities",
			"SQLiteCore",
			"Slate",
			"SlateCore",
			"BlueprintGraph",
			"KismetCompiler",
			"EditorSubsystem",
			"AnimationCore",
			"Niagara",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"Projects",
			"AIModule"
		});

		// --- Conditional: StateTree (UE 5.4+) ---
		bool bHasStateTree = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string StateTreePluginDir = Path.Combine(EngineDir, "Plugins", "Runtime", "StateTree");
			bHasStateTree = Directory.Exists(StateTreePluginDir);

			if (!bHasStateTree)
			{
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
				bHasStateTree = Directory.Exists(Path.Combine(EnginePluginsDir, "StateTree"));
			}
		}

		if (bHasStateTree)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"StateTreeModule",
				"StateTreeEditorModule"
			});
			PublicDefinitions.Add("WITH_STATETREE=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_STATETREE=0");
		}
	}
}
