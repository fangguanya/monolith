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
			// AssetVisual 双 cohort indexer 复用 MonolithCapture 的 IAssetCanonicalRenderer。
			"MonolithCapture",
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
			// AssetVisual indexer GetSupportedClasses 需要 UWidgetBlueprint 的类型。
			"UMG",
			"UMGEditor",
			"AnimationCore",
			"Niagara",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"Projects",
			"AIModule",
			// 渲染 + PNG 编码（AssetVisual indexer 使用）
			"RenderCore",
			"RHI",
			"ImageCore",
			"ImageWrapper"
		});

		// AssetVisual semantic provider 需要 UE NNE（Neural Network Engine）+ NNERuntimeORT 插件。
		// NNE 模块 (Engine/Plugins/Experimental/NNE 在 UE 5.0-5.3 / 升至 Engine/Plugins/NNE 在 5.4+) 始终可作为模块依赖；
		// NNERuntimeORT (DirectML / CPU runtime) 是独立插件，必须在 .uproject 中显式启用。
		// 我们这里只在编译期检测 NNE 模块是否存在；运行期再通过 GetRuntime<INNERuntimeGPU>() 检测 ORT 是否启用。
		string EngineDirForNne = Path.GetFullPath(Target.RelativeEnginePath);
		bool bHasNNE = Directory.Exists(Path.Combine(EngineDirForNne, "Plugins", "NNE"))
			|| Directory.Exists(Path.Combine(EngineDirForNne, "Plugins", "Experimental", "NNE"));
		if (bHasNNE)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"NNE"
			});
			PublicDefinitions.Add("WITH_MONOLITH_NNE=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MONOLITH_NNE=0");
		}

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
