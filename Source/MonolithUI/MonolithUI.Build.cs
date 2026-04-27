using UnrealBuildTool;

public class MonolithUI : ModuleRules
{
    public MonolithUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
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
            "UMG",
            "UMGEditor",
            "Slate",
            "SlateCore",
            "Json",
            "JsonUtilities",
            "KismetCompiler",
            "MovieScene",
            "MovieSceneTracks"
        });
    }
}
