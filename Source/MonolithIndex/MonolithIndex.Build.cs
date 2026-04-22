using UnrealBuildTool;
using System.IO;

/*
 * 这个 Build.cs 文件告诉 Unreal：
 * “MonolithIndex 这个模块编译时要依赖哪些别的模块。”
 *
 * 可以把它理解成一份“配料单”：
 * - Core / Engine 是基础原料；
 * - AssetRegistry / SQLiteCore / Json 是索引功能的常用工具；
 * - Slate / ToolMenus 是编辑器状态栏 UI 需要的东西；
 * - BlueprintGraph / GameplayTags / EnhancedInput 等则是各类 indexer 要读资产时需要的专业模块。
 */
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
			"DerivedDataCache",
			"Json",
			"JsonUtilities",
			"SQLiteCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
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

		// StateTree 在不同引擎版本/打包模式下不一定存在。
		// 这里先探测插件目录，再决定是否把相关模块加进依赖里。
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
