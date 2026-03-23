#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "LevelToolSettings.h"
#include "LevelToolBuildingPool.h"
#include "Delegates/Delegate.h"
#include <atomic>
#include "LevelToolSubsystem.generated.h"

class ALandscape;
class UEditLayerManager;

// ---------------------------------------------------------------------------
//  Progress event for Slate panel updates
// ---------------------------------------------------------------------------
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLevelToolProgress, FString /*Stage*/, float /*Percent*/);
DECLARE_MULTICAST_DELEGATE_OneParam (FOnLevelToolComplete, bool  /*bSuccess*/);
DECLARE_MULTICAST_DELEGATE_OneParam (FOnLevelToolLog,     FString /*Message*/);

// ---------------------------------------------------------------------------
//  Per-job result
// ---------------------------------------------------------------------------
USTRUCT()
struct FLevelToolJobResult
{
    GENERATED_BODY()

    bool   bSuccess          = false;
    FString HeightmapPngPath;
    FString HeightmapR16Path;
    FString BuildingsJsonPath;
    FString RoadsJsonPath;
    FString WaterJsonPath;
    FString PcgCsvPath;
    int32  BuildingCount     = 0;
    float  ElevationMinM     = 0.f;
    float  ElevationMaxM     = 0.f;
    float  EffectiveXYScaleCm = 0.f;  // auto-computed from radius & grid
    FString ErrorMessage;
};

// ---------------------------------------------------------------------------
//  The editor subsystem — one instance per editor session
// ---------------------------------------------------------------------------
UCLASS()
class LEVELTOOL_API ULevelToolSubsystem : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    // ── Lifecycle ────────────────────────────────────────────────────────
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // ── Events ──────────────────────────────────────────────────────────
    FOnLevelToolProgress OnProgress;
    FOnLevelToolComplete OnComplete;
    FOnLevelToolLog      OnLog;

    // ── State ────────────────────────────────────────────────────────────
    bool IsRunning() const { return bJobRunning; }
    const FLevelToolJobResult& GetLastResult() const { return LastResult; }
    const TArray<FString>& GetLogLines() const { return LogLines; }

    // ── Edit Layer Manager (비파괴 레이어 시스템) ────────────────────
    UEditLayerManager* GetEditLayerManager() const { return EditLayerManager; }

    // ── Main API ─────────────────────────────────────────────────────────

    /**
     * Run full pipeline: fetch elevation + buildings, then import into level.
     * @param Preset   Named preset from settings (or empty if using custom coords)
     * @param Lat/Lon  Custom coordinates (used when Preset is empty)
     * @param RadiusKm Area radius in km
     * @param Pool     Building mesh pool DataAsset (can be null = skip actor placement)
     */
    void RunFullPipeline(
        const FString&          Preset,
        float                   Lat,
        float                   Lon,
        float                   RadiusKm,
        ULevelToolBuildingPool* Pool,
        bool                    bSpawnRoads = true);

    void RunLandscapeOnly(const FString& Preset, float Lat, float Lon, float RadiusKm);
    void RunBuildingsOnly (const FString& Preset, float Lat, float Lon, float RadiusKm,
                           ULevelToolBuildingPool* Pool, bool bSpawnRoads = true);

    void CancelJob();
    void ClearGeneratedActors();

    // ── Landscape Import ─────────────────────────────────────────────────
    // ElevationRangeM: actual terrain height range from Python output (0 = use settings default)
    bool ImportHeightmapAsLandscape(const FString& HeightmapPngPath, const FString& LandscapeName,
                                    float ElevationRangeM = 0.f, float OverrideXYScaleCm = 0.f);

    // ── Building Placement ───────────────────────────────────────────────
    int32 PlaceBuildingsFromJson(const FString& JsonPath, ULevelToolBuildingPool* Pool,
                                 float ZOffsetCm = 0.f);

    // ── Utility ──────────────────────────────────────────────────────────
    FString GetPythonScriptPath(const FString& ScriptName) const;
    bool    ValidateSettings(TArray<FString>& OutErrors) const;

private:
    // ── Python Execution ─────────────────────────────────────────────────
    bool RunPythonScript(const FString& ScriptPath, const FString& Args,
                         FString& OutStdout, FString& OutStderr);

    // Builds CLI args string for main.py
    FString BuildMainPyArgs(const FString& Command,
                             const FString& Preset,
                             float Lat, float Lon, float RadiusKm) const;

    // Parse job result from Python stdout
    void ParsePythonOutput(const FString& Stdout, FLevelToolJobResult& OutResult) const;

    // ── Compass markers ──────────────────────────────────────────────────
    void SpawnCompassMarkers(UWorld* World, const FBox& LandscapeBounds);

    // ── Road splines ─────────────────────────────────────────────────────
    void SpawnRoadActors(const FString& JsonPath, const FBox& LandscapeBounds);

    // ── Water bodies ─────────────────────────────────────────────────────
    void SpawnWaterBodies(const FString& JsonPath, const FBox& LandscapeBounds);

    // ── Splat map import ─────────────────────────────────────────────────
    void ImportSplatMapsAsTextures(const FString& HeightmapPngPath, ALandscape* Landscape);

    // ── Map Meta ──────────────────────────────────────────────────────
    void SaveMapMeta(const FString& MapId, float Lat, float Lon, float RadiusKm,
                     const FLevelToolJobResult& Result);
    FString GetEditLayerDir(const FString& MapId) const;

    // ── Building internals ───────────────────────────────────────────────
    struct FBuildingEntry
    {
        int64    OsmId;
        FString  TypeKey;
        float    HeightM;
        float    MinHeightM = 0.f;
        float    AreaM2;
        FVector2D CentroidUE5;    // cm XY
        TArray<FVector2D> FootprintUE5;

        // OSM metadata (optional, from tags)
        FString  Name;
        FString  RoofShape;       // flat, gabled, hipped, dome, pyramidal, etc.
        FString  BuildingColour;  // OSM building:colour (hex or named)
        FString  BuildingMaterial;// concrete, brick, glass, wood, metal, etc.
        int32    Levels = 0;      // building:levels (0 = unknown)
    };

    bool LoadBuildingsJson(const FString& JsonPath, TArray<FBuildingEntry>& OutBuildings);

    AStaticMeshActor* SpawnBuildingActor(const FBuildingEntry& Building,
                                          ULevelToolBuildingPool* Pool,
                                          float ZOffsetCm);

    // Compute scale to match real height using a 1m-tall base mesh
    FVector ComputeBuildingScale(const FBuildingEntry& Building,
                                  UStaticMesh* Mesh) const;

    // ── State ────────────────────────────────────────────────────────────
    std::atomic<bool>   bJobRunning{false};
    std::atomic<uint32> JobGeneration{0};
    FLevelToolJobResult LastResult;
    TArray<FString>     LogLines;

    // Async task handle
    TFuture<void>       AsyncTask;

    // ── Python process management (timeout + cancellation) ────────
    static constexpr double PythonTimeoutSeconds = 300.0;
    FProcHandle         ActivePythonProcess;
    FCriticalSection    PythonProcessLock;

    // Parse structured JSON result block from Python stdout
    bool ParseJsonResultBlock(const FString& Output, FLevelToolJobResult& OutResult) const;

    // Parse __LEVELTOOL_PROGRESS__ JSON line from Python stdout and broadcast
    void ParseAndBroadcastProgress(const FString& JsonStr);

    // ── Cached heightmap for Z lookup (avoids physics line trace timing issue) ─
    TArray<uint16> CachedHeightData;
    int32          CachedHMapWidth  = 0;
    float          CachedZScale     = 0.f;
    float          CachedXYScaleCm  = 0.f;
    float          CachedOriginX    = 0.f;  // landscape actor world X (= -HalfSizeCm)
    float          CachedOriginY    = 0.f;  // landscape actor world Y

    // Returns world-space Z (cm) at given world XY by reading cached heightmap data
    float GetTerrainZAtWorldXY(float WorldX, float WorldY) const;

    // ── Edit Layer Manager ──────────────────────────────────────────────
    UPROPERTY()
    TObjectPtr<UEditLayerManager> EditLayerManager;

    // ── Procedural building material (window grid + type-based tint) ─────
    UPROPERTY()
    TObjectPtr<UMaterial>  CachedBuildingMaterial;
    UPROPERTY()
    TObjectPtr<UTexture2D> CachedWindowTexture;

    void EnsureBuildingMaterial();
    void CreateWindowGridTexture();

    void Log(const FString& Msg);
    void SetProgress(const FString& Stage, float Pct);
    void FinishJob(bool bSuccess);
};
