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
			"Engine",
			// MonolithCaptureUtils.h 暴露 FImage / FCanonicalRenderResult，
			// 下游模块（MonolithIndex / MonolithMesh）include 时必须能解析 FImage 类型。
			"ImageCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"RenderCore",
			"RHI",
			"ImageWrapper",
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
