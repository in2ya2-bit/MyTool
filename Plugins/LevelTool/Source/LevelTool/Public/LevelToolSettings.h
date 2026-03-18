#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "LevelToolSettings.generated.h"

// ---------------------------------------------------------------------------
//  Elevation data source
// ---------------------------------------------------------------------------
UENUM(BlueprintType)
enum class EElevationSource : uint8
{
    OpenElevation    UMETA(DisplayName = "Open-Elevation (Free, SRTM 30m)"),
    OpenTopography   UMETA(DisplayName = "OpenTopography (Free account key, SRTM)"),
    GoogleMaps       UMETA(DisplayName = "Google Maps Elevation API (Paid, highest accuracy)"),
};

// ---------------------------------------------------------------------------
//  Coordinate preset (one named location)
// ---------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FLevelToolCoordPreset
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Location")
    FString Name = TEXT("My Location");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Location",
        meta = (ClampMin = "-90.0", ClampMax = "90.0", UIMin = "-90.0", UIMax = "90.0"))
    float Latitude = 37.5704f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Location",
        meta = (ClampMin = "-180.0", ClampMax = "180.0"))
    float Longitude = 126.9820f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Location",
        meta = (ClampMin = "0.1", ClampMax = "10.0", UIMin = "0.1", UIMax = "5.0"))
    float RadiusKm = 1.0f;
};

// ---------------------------------------------------------------------------
//  Main settings object (stored in Project Settings via Config)
// ---------------------------------------------------------------------------
UCLASS(Config = LevelTool, DefaultConfig, meta = (DisplayName = "Level Tool"))
class LEVELTOOL_API ULevelToolSettings : public UObject
{
    GENERATED_BODY()

public:
    ULevelToolSettings();

    // ── API Keys ────────────────────────────────────────────────────────────
    UPROPERTY(Config, EditAnywhere, Category = "API Keys",
        meta = (DisplayName = "Google Maps API Key",
                ToolTip = "Required for Google Elevation API and 3D Tiles. Get from console.cloud.google.com"))
    FString GoogleMapsApiKey;

    UPROPERTY(Config, EditAnywhere, Category = "API Keys",
        meta = (DisplayName = "OpenTopography API Key",
                ToolTip = "Free key from opentopography.org — gives access to 30m SRTM data"))
    FString OpenTopographyApiKey;

    // ── Python Tool Path ─────────────────────────────────────────────────
    UPROPERTY(Config, EditAnywhere, Category = "Tool Paths",
        meta = (DisplayName = "Python Script Directory",
                ToolTip = "Absolute path to the level_tool Python scripts folder"))
    FDirectoryPath PythonScriptDir;

    UPROPERTY(Config, EditAnywhere, Category = "Tool Paths",
        meta = (DisplayName = "Output Directory",
                ToolTip = "Where generated heightmaps and building JSON are saved"))
    FDirectoryPath OutputDir;

    UPROPERTY(Config, EditAnywhere, Category = "Tool Paths",
        meta = (DisplayName = "Blender Executable Path",
                ToolTip = "Full path to blender.exe for building FBX export"))
    FFilePath BlenderExePath;

    // ── Landscape Settings ──────────────────────────────────────────────
    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Elevation Source"))
    EElevationSource ElevationSource = EElevationSource::OpenElevation;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Heightmap Grid Size",
                ToolTip = "Must be one of: 505, 1009, 2017, 4033 (UE5 valid sizes)",
                ClampMin = "505", ClampMax = "4033"))
    int32 HeightmapSize = 1009;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Z Height Range (meters)",
                ToolTip = "Total vertical range encoded in the heightmap",
                ClampMin = "100", ClampMax = "8000"))
    float ZRangeMeters = 400.0f;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "XY Scale (cm per quad)",
                ToolTip = "1m resolution = 100.0  |  2m resolution = 200.0",
                ClampMin = "50", ClampMax = "400"))
    float XYScaleCm = 100.0f;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Gaussian Smooth Sigma",
                ToolTip = "Higher = smoother terrain. 3.0 recommended for coastal SRTM data. 0 = raw.",
                ClampMin = "0.0", ClampMax = "10.0"))
    float SmoothSigma = 3.0f;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Apply Erosion Simulation",
                ToolTip = "Erosion can create artifacts on flat coastal terrain. Disable for best results."))
    bool bApplyErosion = false;

    UPROPERTY(Config, EditAnywhere, Category = "Landscape",
        meta = (DisplayName = "Erosion Iterations",
                ClampMin = "1", ClampMax = "20",
                EditCondition = "bApplyErosion"))
    int32 ErosionIterations = 3;

    // ── Building Settings ────────────────────────────────────────────────
    UPROPERTY(Config, EditAnywhere, Category = "Buildings",
        meta = (DisplayName = "Min Building Area (m²)",
                ToolTip = "Ignore buildings smaller than this footprint",
                ClampMin = "5", ClampMax = "500"))
    float MinBuildingAreaM2 = 20.0f;

    UPROPERTY(Config, EditAnywhere, Category = "Buildings",
        meta = (DisplayName = "Default Building Height (m)",
                ToolTip = "Used when OSM has no height tag",
                ClampMin = "3", ClampMax = "100"))
    float DefaultBuildingHeightM = 10.0f;

    UPROPERTY(Config, EditAnywhere, Category = "Buildings",
        meta = (DisplayName = "Export Blender FBX",
                ToolTip = "Run Blender headless after building fetch to generate FBX meshes"))
    bool bExportBlenderFbx = true;

    UPROPERTY(Config, EditAnywhere, Category = "Buildings",
        meta = (DisplayName = "Place Actors in Level",
                ToolTip = "Automatically spawn Static Mesh Actors after export"))
    bool bPlaceActorsInLevel = true;

    // ── Saved Presets ────────────────────────────────────────────────────
    UPROPERTY(Config, EditAnywhere, Category = "Presets",
        meta = (DisplayName = "Coordinate Presets",
                ToolTip = "Named locations for quick access"))
    TArray<FLevelToolCoordPreset> CoordPresets;

    // ── Helpers ──────────────────────────────────────────────────────────
    static ULevelToolSettings* Get()
    {
        return GetMutableDefault<ULevelToolSettings>();
    }

    // Compute UE5 Z scale from Z range
    float GetLandscapeZScale() const
    {
        // UE5 encodes -256..+256 units at ZScale=1.0
        // Total range = ZScale * 512 cm → ZScale = ZRangeMeters*100 / 512
        return (ZRangeMeters * 100.0f) / 512.0f;
    }
};
