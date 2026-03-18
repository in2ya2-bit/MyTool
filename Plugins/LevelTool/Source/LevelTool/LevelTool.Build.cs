using UnrealBuildTool;

public class LevelTool : ModuleRules
{
    public LevelTool(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Landscape",
            "LandscapeEditor",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // Rendering utilities
            "RenderCore",

            // Editor framework
            "UnrealEd",
            "EditorFramework",
            "EditorSubsystem",
            "EditorStyle",
            "EditorWidgets",

            // Slate UI
            "Slate",
            "SlateCore",
            "ToolMenus",
            "WorkspaceMenuStructure",

            // Python integration (runs our terrain/building scripts)
            "PythonScriptPlugin",

            // Content utilities
            "AssetTools",
            "AssetRegistry",
            "ContentBrowser",
            "EditorScriptingUtilities",

            // Level / actor utilities
            "LevelEditor",
            "PropertyEditor",

            // HTTP for API calls
            "HTTP",
            "Json",
            "JsonUtilities",

            // PCG integration
            "PCG",
            "PCGEditor",

            // Image IO (heightmap preview)
            "ImageWrapper",

            // Input
            "InputCore",

            // Clipboard
            "ApplicationCore",

            // Road geometry generation
            "ProceduralMeshComponent",
        });

        // Allow access to private engine headers for landscape APIs
        PrivateIncludePaths.AddRange(new string[]
        {
            "LevelTool/Private",
        });
    }
}
