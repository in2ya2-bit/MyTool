using UnrealBuildTool;

public class BuildingGeneratorEditor : ModuleRules
{
    public BuildingGeneratorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "BuildingGenerator",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",
            "EditorFramework",
            "EditorSubsystem",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "InputCore",
            "PropertyEditor",
            "LevelEditor",
            "WorkspaceMenuStructure",
            "EditorWidgets",
            "ApplicationCore",
            "AssetTools",
        });
    }
}
