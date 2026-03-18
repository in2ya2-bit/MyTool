#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"

#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

// Rendering
#include "RenderingThread.h"

// UE5 Landscape
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeEdit.h"
#include "LandscapeComponent.h"

// Image IO
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

// UE5 Level utilities
#include "Engine/World.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EditorLevelLibrary.h"
#include "LevelEditor.h"
#include "EngineUtils.h"

// Asset import
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Containers/Ticker.h"

// JSON parsing for buildings.json
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Transaction support (Ctrl+Z)
#include "ScopedTransaction.h"

// Python plugin integration
#include "IPythonScriptPlugin.h"


DEFINE_LOG_CATEGORY_STATIC(LogLevelTool, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogLevelTool, Log, TEXT("LevelTool subsystem initialized"));
}

void ULevelToolSubsystem::Deinitialize()
{
    if (bJobRunning)
        CancelJob();

    Super::Deinitialize();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public pipeline entry points
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::RunFullPipeline(
    const FString& Preset, float Lat, float Lon, float RadiusKm,
    ULevelToolBuildingPool* Pool, bool bSpawnRoads)
{
    if (bJobRunning)
    {
        Log(TEXT("⚠ A job is already running. Cancel it first."));
        return;
    }

    bJobRunning = true;
    LogLines.Empty();
    LastResult = FLevelToolJobResult();

    Log(FString::Printf(TEXT("▶ Starting full pipeline  preset='%s'  lat=%.4f  lon=%.4f  r=%.1fkm"),
        *Preset, Lat, Lon, RadiusKm));

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ULevelToolBuildingPool> WeakPool(Pool);

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool, bSpawnRoads]()
    {
        ULevelToolSubsystem* Self = WeakThis.Get();
        if (!Self) return;

        FLevelToolJobResult Result;

        // ── Step 1: Run Python main.py all ──────────────────────────────
        Self->SetProgress(TEXT("Fetching terrain and building data..."), 0.05f);

        FString Args     = Self->BuildMainPyArgs(TEXT("all"), Preset, Lat, Lon, RadiusKm);
        FString MainPy   = Self->GetPythonScriptPath(TEXT("main.py"));
        FString Stdout, Stderr;

        if (!Self->RunPythonScript(MainPy, Args, Stdout, Stderr))
        {
            Result.bSuccess     = false;
            Result.ErrorMessage = FString::Printf(TEXT("Python error:\n%s"), *Stderr);
            Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Result]()
            {
                if (ULevelToolSubsystem* S = WeakThis.Get())
                {
                    S->LastResult = Result;
                    S->Log(TEXT("✖ Python pipeline failed: ") + Result.ErrorMessage);
                    S->FinishJob(false);
                }
            });
            return;
        }

        Self->ParsePythonOutput(Stdout + TEXT("\n") + Stderr, Result);

        // ── Fallback path resolution ──────────────────────────────────────
        // If parsing missed or the parsed path has a space/encoding artifact,
        // construct the expected path from the known preset name + script dir.
        {
            FString ScriptDir = FPaths::GetPath(Self->GetPythonScriptPath(TEXT("main.py")));
            FString MapName   = Preset.IsEmpty()
                ? FString::Printf(TEXT("custom_%.3f_%.3f"), Lat, Lon)
                : Preset;

            auto TryFallback = [&](FString& Path, const FString& SubDir, const FString& Suffix)
            {
                if (!Path.IsEmpty() && FPaths::FileExists(Path)) return; // parsed path is good
                FString Expected = FPaths::Combine(ScriptDir, TEXT("output"), SubDir, MapName + Suffix);
                if (FPaths::FileExists(Expected))
                {
                    Self->Log(FString::Printf(TEXT("  (fallback path: %s)"), *Expected));
                    Path = Expected;
                }
            };

            TryFallback(Result.HeightmapPngPath,   TEXT("heightmaps"), TEXT("_heightmap.png"));
            TryFallback(Result.BuildingsJsonPath,   TEXT("buildings"),  TEXT("_buildings.json"));
            TryFallback(Result.RoadsJsonPath,       TEXT("buildings"),  TEXT("_roads.json"));
        }

        // ── Step 2: Import Landscape (game thread) ───────────────────────
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, WeakPool, Result, bSpawnRoads]() mutable
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            if (!Self) return;

            Self->SetProgress(TEXT("Importing landscape..."), 0.50f);

            if (!Result.HeightmapPngPath.IsEmpty())
            {
                FString LandscapeName = FPaths::GetBaseFilename(Result.HeightmapPngPath)
                                          .Replace(TEXT("_heightmap"), TEXT(""));
                float ElevRange = (Result.ElevationMaxM > Result.ElevationMinM)
                    ? (Result.ElevationMaxM - Result.ElevationMinM) : 0.f;
                if (!Self->ImportHeightmapAsLandscape(Result.HeightmapPngPath, LandscapeName, ElevRange))
                {
                    Self->Log(TEXT("⚠ Landscape import failed — check heightmap path"));
                }
                else
                {
                    Self->Log(TEXT("✔ Landscape imported: ") + LandscapeName);
                }
            }

            // ── Step 3: Place Buildings ──────────────────────────────────
            if (!Result.BuildingsJsonPath.IsEmpty() && WeakPool.IsValid())
            {
                Self->SetProgress(TEXT("Placing buildings..."), 0.75f);
                int32 Count = Self->PlaceBuildingsFromJson(Result.BuildingsJsonPath, WeakPool.Get(), 0.f);
                Result.BuildingCount = Count;
                Self->Log(FString::Printf(TEXT("✔ Placed %d building actors"), Count));
            }
            else if (!WeakPool.IsValid())
            {
                Self->Log(TEXT("ℹ No BuildingPool assigned — skipping actor placement"));
            }

            // ── Step 4: Spawn Road Splines ───────────────────────────────
            // Get Landscape bounds (shared by roads + water)
            UWorld* PostWorld = GEditor->GetEditorWorldContext().World();
            FBox LandscapeBounds(ForceInit);
            if (PostWorld)
            {
                for (TActorIterator<ALandscape> It(PostWorld); It; ++It)
                {
                    LandscapeBounds = It->GetComponentsBoundingBox(true);
                    break;
                }
            }

            if (bSpawnRoads && !Result.RoadsJsonPath.IsEmpty())
            {
                Self->SetProgress(TEXT("Spawning roads..."), 0.90f);
                Self->SpawnRoadActors(Result.RoadsJsonPath, LandscapeBounds);
            }

Self->LastResult = Result;
            Self->SetProgress(TEXT("Done"), 1.0f);
            Self->FinishJob(true);
        });
    });
}

void ULevelToolSubsystem::RunLandscapeOnly(
    const FString& Preset, float Lat, float Lon, float RadiusKm)
{
    RunFullPipeline(Preset, Lat, Lon, RadiusKm, nullptr, false);
}

void ULevelToolSubsystem::RunBuildingsOnly(
    const FString& Preset, float Lat, float Lon, float RadiusKm,
    ULevelToolBuildingPool* Pool, bool bSpawnRoads)
{
    if (bJobRunning)
    {
        Log(TEXT("⚠ A job is already running."));
        return;
    }

    bJobRunning = true;
    LogLines.Empty();

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ULevelToolBuildingPool> WeakPool(Pool);

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool, bSpawnRoads]()
    {
        ULevelToolSubsystem* Self = WeakThis.Get();
        if (!Self) return;

        FLevelToolJobResult Result;
        Self->SetProgress(TEXT("Fetching OSM building data..."), 0.1f);

        FString Args   = Self->BuildMainPyArgs(TEXT("buildings"), Preset, Lat, Lon, RadiusKm);
        FString MainPy = Self->GetPythonScriptPath(TEXT("main.py"));
        FString Stdout, Stderr;

        if (!Self->RunPythonScript(MainPy, Args, Stdout, Stderr))
        {
            Result.bSuccess     = false;
            Result.ErrorMessage = Stderr;
            Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Result]() mutable
            {
                if (ULevelToolSubsystem* S = WeakThis.Get())
                {
                    S->LastResult = Result;
                    S->FinishJob(false);
                }
            });
            return;
        }

        Self->ParsePythonOutput(Stdout + TEXT("\n") + Stderr, Result);

        // Fallback: construct expected JSON path if parsing failed
        {
            FString ScriptDir = FPaths::GetPath(Self->GetPythonScriptPath(TEXT("main.py")));
            FString MapName   = Preset.IsEmpty()
                ? FString::Printf(TEXT("custom_%.3f_%.3f"), Lat, Lon)
                : Preset;
            if (Result.BuildingsJsonPath.IsEmpty() || !FPaths::FileExists(Result.BuildingsJsonPath))
            {
                FString Expected = FPaths::Combine(ScriptDir, TEXT("output"), TEXT("buildings"),
                    MapName + TEXT("_buildings.json"));
                if (FPaths::FileExists(Expected))
                {
                    Self->Log(FString::Printf(TEXT("  (fallback path: %s)"), *Expected));
                    Result.BuildingsJsonPath = Expected;
                }
            }
        }

        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, WeakPool, Result, bSpawnRoads]() mutable
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            if (!Self) return;

            if (!Result.BuildingsJsonPath.IsEmpty() && WeakPool.IsValid())
            {
                Self->SetProgress(TEXT("Placing buildings..."), 0.7f);
                int32 Count = Self->PlaceBuildingsFromJson(Result.BuildingsJsonPath, WeakPool.Get(), 0.f);
                Result.BuildingCount = Count;
                Self->Log(FString::Printf(TEXT("✔ Placed %d buildings"), Count));
            }

            if (bSpawnRoads && !Result.RoadsJsonPath.IsEmpty())
            {
                Self->SetProgress(TEXT("Spawning roads..."), 0.90f);
                UWorld* RoadWorld = GEditor->GetEditorWorldContext().World();
                FBox LandscapeBounds(ForceInit);
                if (RoadWorld)
                {
                    for (TActorIterator<ALandscape> It(RoadWorld); It; ++It)
                    {
                        LandscapeBounds = It->GetComponentsBoundingBox(true);
                        break;
                    }
                }
                Self->SpawnRoadActors(Result.RoadsJsonPath, LandscapeBounds);
            }

            Self->LastResult = Result;
            Self->SetProgress(TEXT("Done"), 1.0f);
            Self->FinishJob(true);
        });
    });
}

void ULevelToolSubsystem::CancelJob()
{
    if (bJobRunning)
    {
        Log(TEXT("⚠ Job cancelled"));
        bJobRunning = false;
        OnComplete.Broadcast(false);
    }
}

void ULevelToolSubsystem::ClearGeneratedActors()
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "ClearActors", "LevelTool: Clear Generated Actors"));

    TArray<AActor*> ToDelete;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        // Any actor inside the LevelTool outliner folder
        if (Actor->GetFolderPath().ToString().StartsWith(TEXT("LevelTool")))
        {
            ToDelete.Add(Actor);
            continue;
        }
        // Landscape tagged by this tool
        if (Actor->Tags.Contains(TEXT("LevelToolGenerated")))
        {
            ToDelete.Add(Actor);
        }
    }

    int32 Count = ToDelete.Num();
    for (AActor* A : ToDelete)
    {
        A->Modify();
        World->DestroyActor(A);
    }

    Log(FString::Printf(TEXT("✔ Cleared %d LevelTool actors"), Count));
    GEditor->RedrawAllViewports();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Python Execution
// ─────────────────────────────────────────────────────────────────────────────

bool ULevelToolSubsystem::RunPythonScript(
    const FString& ScriptPath, const FString& Args,
    FString& OutStdout, FString& OutStderr)
{
    if (!FPaths::FileExists(ScriptPath))
    {
        OutStderr = FString::Printf(TEXT("Script not found: %s"), *ScriptPath);
        return false;
    }

    // Find python executable — prefer UE5 bundled Python, fall back to system python
    FString PythonExe;
#if PLATFORM_WINDOWS
    {
        // UE5 bundled Python (always available, no PATH dependency)
        FString EnginePython = FPaths::ConvertRelativePathToFull(
            FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
        if (FPaths::FileExists(EnginePython))
        {
            PythonExe = EnginePython;
        }
        else
        {
            PythonExe = TEXT("python");
        }
    }
#else
    PythonExe = TEXT("python3");
#endif

    // Pass the script directory so local packages (landscape/, buildings/ etc.) are importable
    FString ScriptDir = FPaths::GetPath(ScriptPath);
    // -X utf8 forces UTF-8 I/O on Windows (prevents cp949 UnicodeEncodeError)
    FString CommandLine = FString::Printf(TEXT("-X utf8 -u \"%s\" %s"), *ScriptPath, *Args);

    Log(FString::Printf(TEXT("$ %s %s"), *PythonExe, *CommandLine));

    int32  ReturnCode = 0;
    FString Stdout, Stderr;

    bool bOk = FPlatformProcess::ExecProcess(
        *PythonExe,
        *CommandLine,
        &ReturnCode,
        &Stdout,
        &Stderr,
        *ScriptDir   // working directory = files/ folder
    );

    // Log each output line
    TArray<FString> Lines;
    Stdout.ParseIntoArrayLines(Lines);
    for (const FString& Line : Lines)
    {
        if (!Line.IsEmpty())
            Log(Line);
    }

    OutStdout = Stdout;
    OutStderr = Stderr;

    if (!bOk || ReturnCode != 0)
    {
        Log(FString::Printf(TEXT("✖ Python exited with code %d"), ReturnCode));
        if (!Stderr.IsEmpty())
            Log(TEXT("stderr: ") + Stderr);
        return false;
    }

    return true;
}

FString ULevelToolSubsystem::BuildMainPyArgs(
    const FString& Command,
    const FString& Preset,
    float Lat, float Lon, float RadiusKm) const
{
    const ULevelToolSettings* S = ULevelToolSettings::Get();
    FString Args = Command;

    // Always pass explicit coordinates — no Python-side preset lookup needed.
    // Pass --name so output files use the preset name (or a fallback).

    // Auto-match building fetch radius to Landscape XY size so buildings never exceed the terrain.
    // LandscapeHalfSize = (HeightmapSize - 1) * XYScaleCm / 2  (in cm → convert to km)
    float LandscapeHalfKm = (S->HeightmapSize - 1) * S->XYScaleCm / 2.0f / 100.0f / 1000.0f;
    float EffectiveRadius  = FMath::Min(RadiusKm, LandscapeHalfKm);

    Args += FString::Printf(TEXT(" --lat %.6f --lon %.6f --radius %.4f"), Lat, Lon, EffectiveRadius);
    if (!Preset.IsEmpty())
        Args += FString::Printf(TEXT(" --name %s"), *Preset);

    // Elevation source
    if (Command != TEXT("buildings"))
    {
        switch (S->ElevationSource)
        {
        case EElevationSource::OpenTopography:
            Args += TEXT(" --elevation-source opentopography");
            if (!S->OpenTopographyApiKey.IsEmpty())
                Args += FString::Printf(TEXT(" --api-key %s"), *S->OpenTopographyApiKey);
            break;
        case EElevationSource::GoogleMaps:
            Args += TEXT(" --elevation-source google");
            if (!S->GoogleMapsApiKey.IsEmpty())
                Args += FString::Printf(TEXT(" --api-key %s"), *S->GoogleMapsApiKey);
            break;
        default:
            Args += TEXT(" --elevation-source open_elevation");
            break;
        }

        Args += FString::Printf(TEXT(" --smooth-sigma %.2f"), S->SmoothSigma);
        Args += FString::Printf(TEXT(" --erosion-iters %d"), S->ErosionIterations);
        if (!S->bApplyErosion)
            Args += TEXT(" --no-erosion");
        // Water mask is always fetched unless user disables it via settings (future opt-in)
        // Currently always enabled; add --no-water here if a toggle is added to settings.
    }

    return Args;
}

void ULevelToolSubsystem::ParsePythonOutput(
    const FString& Stdout, FLevelToolJobResult& OutResult) const
{
    const ULevelToolSettings* S = ULevelToolSettings::Get();

    // Search stdout for file paths written by Python
    TArray<FString> Lines;
    Stdout.ParseIntoArrayLines(Lines);

    // Helper: extract full path from "Key: /full/path  (optional suffix)"
    auto ExtractPath = [](const FString& Line, const FString& Prefix) -> FString
    {
        int32 Idx = Line.Find(Prefix);
        if (Idx == INDEX_NONE) return FString();
        FString After = Line.Mid(Idx + Prefix.Len()).TrimStart();
        // Path ends at first double-space or end of string
        int32 SpaceIdx = After.Find(TEXT("  "));
        if (SpaceIdx != INDEX_NONE)
            After = After.Left(SpaceIdx);
        return After.TrimStartAndEnd();
    };

    for (const FString& Line : Lines)
    {
        // "  3. File: E:\...\Seoul_Jongno_heightmap.png"  (from UE5 import instructions)
        if (OutResult.HeightmapPngPath.IsEmpty()
            && Line.Contains(TEXT("_heightmap.png"))
            && Line.Contains(TEXT("File:")))
        {
            OutResult.HeightmapPngPath = ExtractPath(Line, TEXT("File:"));
        }
        // "  Heightmap PNG saved: E:\...\Seoul_Jongno_heightmap.png  (1009x1009)"  (stderr forwarded)
        else if (OutResult.HeightmapPngPath.IsEmpty()
                 && Line.Contains(TEXT("Heightmap PNG saved:")))
        {
            OutResult.HeightmapPngPath = ExtractPath(Line, TEXT("Heightmap PNG saved:"));
        }
        // "  Buildings JSON : E:\...\Seoul_Jongno_buildings.json"
        if (OutResult.BuildingsJsonPath.IsEmpty()
            && Line.Contains(TEXT("_buildings.json"))
            && Line.Contains(TEXT("Buildings JSON")))
        {
            // Try "Buildings JSON : path" format first
            FString Path = ExtractPath(Line, TEXT("Buildings JSON :"));
            if (Path.IsEmpty())
                Path = ExtractPath(Line, TEXT("Buildings JSON saved:"));
            if (!Path.IsEmpty())
                OutResult.BuildingsJsonPath = Path;
        }
        // "  Roads JSON     : E:\...\Seoul_Jongno_roads.json"
        if (OutResult.RoadsJsonPath.IsEmpty()
            && Line.Contains(TEXT("_roads.json"))
            && Line.Contains(TEXT("Roads JSON")))
        {
            FString Path = ExtractPath(Line, TEXT("Roads JSON :"));
            if (Path.IsEmpty())
                Path = ExtractPath(Line, TEXT("Roads JSON saved:"));
            if (!Path.IsEmpty())
                OutResult.RoadsJsonPath = Path;
        }
        // "Elevation stats : min=-2.30m  max=8.50m ..."  (negative values supported)
        if (Line.Contains(TEXT("Elevation stats")))
        {
            FRegexMatcher MinM(FRegexPattern(TEXT("min=(-?[0-9]+\\.?[0-9]*)m")), Line);
            FRegexMatcher MaxM(FRegexPattern(TEXT("max=(-?[0-9]+\\.?[0-9]*)m")), Line);
            if (MinM.FindNext()) OutResult.ElevationMinM = FCString::Atof(*MinM.GetCaptureGroup(1));
            if (MaxM.FindNext()) OutResult.ElevationMaxM = FCString::Atof(*MaxM.GetCaptureGroup(1));
        }
        // "Elevation min   : -2.30m"  (dedicated lines for reliable parsing)
        if (Line.Contains(TEXT("Elevation min")))
        {
            FRegexMatcher M(FRegexPattern(TEXT(":.*?(-?[0-9]+\\.?[0-9]*)m")), Line);
            if (M.FindNext()) OutResult.ElevationMinM = FCString::Atof(*M.GetCaptureGroup(1));
        }
        if (Line.Contains(TEXT("Elevation max")))
        {
            FRegexMatcher M(FRegexPattern(TEXT(":.*?(-?[0-9]+\\.?[0-9]*)m")), Line);
            if (M.FindNext()) OutResult.ElevationMaxM = FCString::Atof(*M.GetCaptureGroup(1));
        }
        // "Heightmap PNG   : E:\...\name_heightmap.png"
        if (OutResult.HeightmapPngPath.IsEmpty() && Line.Contains(TEXT("Heightmap PNG")))
        {
            FString Path = ExtractPath(Line, TEXT("Heightmap PNG   :"));
            if (Path.IsEmpty()) Path = ExtractPath(Line, TEXT("Heightmap PNG :"));
            if (!Path.IsEmpty()) OutResult.HeightmapPngPath = Path;
        }
    }

    // Log what was parsed for diagnostics
    if (!OutResult.HeightmapPngPath.IsEmpty())
    {
        UE_LOG(LogLevelTool, Log, TEXT("ParsePythonOutput: heightmap = %s"), *OutResult.HeightmapPngPath);
    }
    else
    {
        UE_LOG(LogLevelTool, Warning, TEXT("ParsePythonOutput: heightmap path not found in stdout"));
    }

    if (!OutResult.BuildingsJsonPath.IsEmpty())
    {
        UE_LOG(LogLevelTool, Log, TEXT("ParsePythonOutput: buildings = %s"), *OutResult.BuildingsJsonPath);
    }

    OutResult.bSuccess = !OutResult.HeightmapPngPath.IsEmpty() ||
                         !OutResult.BuildingsJsonPath.IsEmpty();
}

FString ULevelToolSubsystem::GetPythonScriptPath(const FString& ScriptName) const
{
    const ULevelToolSettings* S = ULevelToolSettings::Get();
    return FPaths::Combine(S->PythonScriptDir.Path, ScriptName);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Landscape Import
// ─────────────────────────────────────────────────────────────────────────────

bool ULevelToolSubsystem::ImportHeightmapAsLandscape(
    const FString& HeightmapPngPath, const FString& LandscapeName, float ElevationRangeM)
{
    if (!FPaths::FileExists(HeightmapPngPath))
    {
        Log(FString::Printf(TEXT("✖ Heightmap not found: %s"), *HeightmapPngPath));
        return false;
    }

    const ULevelToolSettings* S = ULevelToolSettings::Get();

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Log(TEXT("✖ No editor world available"));
        return false;
    }

    // Landscape component layout for 1009×1009 heightmap
    // 1009 = 15 components × 4 sections × 16 quads + 1 = 961 + 48 = 1009
    const int32 SectionSize       = 63;    // quads per section
    const int32 SectionsPerComp   = 1;
    const int32 QuadsPerComp      = SectionSize * SectionsPerComp;
    const int32 TargetSize        = S->HeightmapSize - 1;  // vertices → quads
    const int32 ComponentCountX   = FMath::CeilToInt((float)TargetSize / QuadsPerComp);
    const int32 ComponentCountY   = ComponentCountX;

    // Prefer .r16 raw binary (little-endian uint16) — more reliable than PNG 16-bit decoding
    const int32 NumVerts = S->HeightmapSize * S->HeightmapSize;
    TArray<uint16> HeightData;
    HeightData.SetNumUninitialized(NumVerts);

    FString R16Path = FPaths::ChangeExtension(HeightmapPngPath, TEXT("r16"));
    bool bLoadedFromR16 = false;

    if (FPaths::FileExists(R16Path))
    {
        TArray<uint8> RawR16;
        if (FFileHelper::LoadFileToArray(RawR16, *R16Path))
        {
            const int32 ExpectedBytes = NumVerts * sizeof(uint16);
            if (RawR16.Num() == ExpectedBytes)
            {
                FMemory::Memcpy(HeightData.GetData(), RawR16.GetData(), ExpectedBytes);
                bLoadedFromR16 = true;
                Log(FString::Printf(TEXT("  Loaded R16: %s  (%d bytes)"), *R16Path, RawR16.Num()));
            }
            else
            {
                Log(FString::Printf(TEXT("⚠ R16 size mismatch: got %d bytes, expected %d"),
                    RawR16.Num(), ExpectedBytes));
            }
        }
    }

    if (!bLoadedFromR16)
    {
        // Fall back: decode PNG via ImageWrapper
        TArray<uint8> RawPng;
        if (!FFileHelper::LoadFileToArray(RawPng, *HeightmapPngPath))
        {
            Log(TEXT("✖ Failed to read heightmap file (neither R16 nor PNG found)"));
            return false;
        }

        IImageWrapperModule& IWM =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);

        if (!IW->SetCompressed(RawPng.GetData(), RawPng.Num()))
        {
            Log(TEXT("✖ Failed to decode PNG heightmap"));
            return false;
        }

        TArray<uint8> Uncompressed;
        if (!IW->GetRaw(ERGBFormat::Gray, 16, Uncompressed))
        {
            Log(TEXT("✖ Failed to get 16-bit grayscale from PNG"));
            return false;
        }

        if (Uncompressed.Num() < NumVerts * (int32)sizeof(uint16))
        {
            Log(FString::Printf(TEXT("✖ PNG data too small: %d bytes for %d verts"),
                Uncompressed.Num(), NumVerts));
            return false;
        }

        FMemory::Memcpy(HeightData.GetData(), Uncompressed.GetData(), NumVerts * sizeof(uint16));
        Log(TEXT("  Loaded PNG (16-bit fallback)"));
    }

    // ── Diagnostic: verify height data is non-flat ──────────────────────────
    {
        uint16 MinH = 65535, MaxH = 0;
        for (int32 i = 0; i < HeightData.Num(); ++i)
        {
            MinH = FMath::Min(MinH, HeightData[i]);
            MaxH = FMath::Max(MaxH, HeightData[i]);
        }
        Log(FString::Printf(TEXT("  HeightData uint16 range: min=%u  max=%u  (flat if both=32768)"), MinH, MaxH));
    }

    // Apply scale BEFORE Import() so normals are computed with correct Z scale.
    // (Matches UE5 editor NewLandscape path: SetActorRelativeScale3D → Import())
    const float ActualRangeM = (ElevationRangeM > 1.f)
        ? FMath::Clamp(ElevationRangeM, 1.f, 400.f)
        : S->ZRangeMeters;
    const float ZScale = ActualRangeM * 100.f / 512.f;
    const float ActorZ = 32768.f * ZScale / 128.f;

    // Center the landscape at world origin so that building/road coordinates
    // (which are relative to the query center = (0,0)) land in the middle of
    // the landscape, not at its corner.
    // Pixel (0,0) = NW corner = actor position.
    // Landscape total width = (HMapWidth-1) * XYScaleCm, so half = offset below.
    const float HalfSizeCm = (S->HeightmapSize - 1) * S->XYScaleCm / 2.0f;
    const float ActorX     = -HalfSizeCm;
    const float ActorY     = -HalfSizeCm;

    // Create landscape actor
    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "ImportLandscape", "LevelTool: Import Landscape"));

    FActorSpawnParameters SpawnParams;
    SpawnParams.bNoFail = true;

    ALandscape* Landscape = World->SpawnActor<ALandscape>(
        ALandscape::StaticClass(), FVector(ActorX, ActorY, ActorZ), FRotator::ZeroRotator, SpawnParams);
    if (!Landscape)
    {
        Log(TEXT("✖ Failed to spawn Landscape actor"));
        return false;
    }

    Landscape->SetActorRelativeScale3D(FVector(S->XYScaleCm, S->XYScaleCm, ZScale));
    Landscape->SetActorLabel(LandscapeName);
    Landscape->Tags.AddUnique(TEXT("LevelToolGenerated"));

    Log(FString::Printf(TEXT("  ElevationRangeM: %.2fm  ZScale: %.4f  ActorXY: (%.0f, %.0f)  ActorZ: %.0fcm"),
        ActualRangeM, ZScale, ActorX, ActorY, ActorZ));

    // Keep a copy for direct HeightmapTexture write (see below).
    TArray<uint16> HeightDataCopy = HeightData;
    const int32    HMapWidth      = S->HeightmapSize;   // 1009

    // Pass real data to Import() so edit-layer texture is populated.
    TMap<FGuid, TArray<uint16>> HeightDataMap;
    HeightDataMap.Add(FGuid(), MoveTemp(HeightData));

    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> EmptyLayerInfos;
    EmptyLayerInfos.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

    TArray<FLandscapeLayer> EmptyLayers;

    Landscape->Import(
        FGuid::NewGuid(),
        0, 0,
        ComponentCountX * QuadsPerComp,
        ComponentCountY * QuadsPerComp,
        SectionsPerComp,
        SectionSize,
        HeightDataMap,
        nullptr,
        EmptyLayerInfos,
        ELandscapeImportAlphamapType::Additive,
        TArrayView<const FLandscapeLayer>(EmptyLayers)
    );

    // ── Direct HeightmapTexture write ────────────────────────────────────────
    // UE5.7 EditLayers: the final HeightmapTexture used by the GPU renderer is
    // produced by a GPU composite that runs asynchronously.  Until it runs the
    // mesh is flat.  We bypass this by writing the height values directly into
    // every component's HeightmapTexture (BGRA8: R=H>>8, G=H&0xFF, B/A=128).
    // Format confirmed: LandscapeDataAccess.h  GetHeight = (R<<8)|G.
    {
        ULandscapeInfo* LInfo = Landscape->GetLandscapeInfo();
        if (LInfo)
        {
            LInfo->ForAllLandscapeComponents([&](ULandscapeComponent* Comp)
            {
                UTexture2D* HTex = Comp->GetHeightmap(/*bReturnEditingHeightmap=*/false);
                if (!HTex || !HTex->GetPlatformData()) return;

                FTexture2DMipMap& Mip = HTex->GetPlatformData()->Mips[0];
                if (Mip.BulkData.GetBulkDataSize() == 0) return;

                const int32 TexW = Mip.SizeX;
                const int32 TexH = Mip.SizeY;

                // Pixel origin of this component inside the shared texture.
                // HeightmapScaleBias.ZW = UV offset  (offset / texSize).
                const int32 OffX = FMath::RoundToInt32(
                    static_cast<float>(Comp->HeightmapScaleBias.Z) * TexW);
                const int32 OffY = FMath::RoundToInt32(
                    static_cast<float>(Comp->HeightmapScaleBias.W) * TexH);

                const int32   CompVerts = Comp->ComponentSizeQuads + 1;
                const FIntPoint Base    = Comp->GetSectionBase();

                FColor* Pixels = static_cast<FColor*>(
                    Mip.BulkData.Lock(LOCK_READ_WRITE));
                if (!Pixels)
                {
                    Mip.BulkData.Unlock();
                    return;
                }

                for (int32 LY = 0; LY < CompVerts; ++LY)
                {
                    for (int32 LX = 0; LX < CompVerts; ++LX)
                    {
                        const int32 DataX   = Base.X + LX;
                        const int32 DataY   = Base.Y + LY;
                        const int32 DataIdx = DataY * HMapWidth + DataX;
                        if (DataIdx < 0 || DataIdx >= HeightDataCopy.Num()) continue;

                        const uint16 H    = HeightDataCopy[DataIdx];
                        const int32  PixX = OffX + LX;
                        const int32  PixY = OffY + LY;
                        if (PixX < 0 || PixX >= TexW || PixY < 0 || PixY >= TexH) continue;

                        FColor& P = Pixels[PixY * TexW + PixX];
                        P.R = static_cast<uint8>(H >> 8);
                        P.G = static_cast<uint8>(H & 0xFF);
                        P.B = 128;  // flat normal placeholder
                        P.A = 128;
                    }
                }

                Mip.BulkData.Unlock();
                HTex->UpdateResource();
                Comp->MarkRenderStateDirty();
            });

            Log(TEXT("  ✔ HeightmapTexture written directly — terrain should be visible"));
        }
    }

    // Cache heightmap data for direct Z lookup during building/road placement.
    // (Landscape physics collision is cooked asynchronously — line traces right
    //  after spawn would miss the terrain.  Reading the array directly is instant.)
    CachedHeightData  = HeightDataCopy;
    CachedHMapWidth   = HMapWidth;
    CachedZScale      = ZScale;
    CachedXYScaleCm   = S->XYScaleCm;
    CachedOriginX     = ActorX;
    CachedOriginY     = ActorY;

    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (LandscapeInfo)
    {
        LandscapeInfo->UpdateLayerInfoMap(Landscape);
        ULandscapeInfo::RecreateLandscapeInfo(World, true);
    }

    // Select and focus viewport
    GEditor->SelectNone(false, true);
    GEditor->SelectActor(Landscape, true, true);
    GEditor->NoteSelectionChange();
    GEditor->RedrawAllViewports();

    Log(FString::Printf(TEXT("✔ Landscape created: %s  scale=%.0f×%.0f×%.4f  components=%d×%d"),
        *LandscapeName,
        S->XYScaleCm, S->XYScaleCm, ZScale,
        ComponentCountX, ComponentCountY));

    // Import splat maps (grass/rock/sand/snow) as texture assets
    ImportSplatMapsAsTextures(HeightmapPngPath, Landscape);

    // Spawn compass markers so the user can identify map center + cardinal directions
    FBox LandscapeBounds = Landscape->GetComponentsBoundingBox(true);
    SpawnCompassMarkers(World, LandscapeBounds);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Road Splines
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::SpawnRoadActors(const FString& JsonPath, const FBox& LandscapeBounds)
{
    if (!FPaths::FileExists(JsonPath))
    {
        Log(FString::Printf(TEXT("  ⚠ Roads JSON not found: %s"), *JsonPath));
        return;
    }

    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return;

    TArray<TSharedPtr<FJsonValue>> JsonArray;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, JsonArray)) return;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    UMaterial* BaseMat = LoadObject<UMaterial>(nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    // Road properties per type
    struct FRoadProps { float HalfWidthCm; FLinearColor Color; };
    auto GetProps = [](const FString& Type) -> FRoadProps
    {
        if (Type == TEXT("major")) return { 600.f, FLinearColor(0.08f, 0.08f, 0.08f) }; // 12m, dark asphalt
        if (Type == TEXT("path"))  return { 100.f, FLinearColor(0.55f, 0.50f, 0.38f) }; //  2m, dirt/sand
        return                            { 300.f, FLinearColor(0.18f, 0.18f, 0.18f) }; //  6m, asphalt
    };

    // Geometry accumulator per road type — all roads merged into 3 ProceduralMesh actors
    struct FRoadGeom
    {
        TArray<FVector>     Vertices;
        TArray<int32>       Triangles;
        TArray<FVector>     Normals;
        TArray<FVector2D>   UVs;
        float               UOffset = 0.f;
    };
    TMap<FString, FRoadGeom> Geoms;
    Geoms.Add(TEXT("major"), {});
    Geoms.Add(TEXT("minor"), {});
    Geoms.Add(TEXT("path"),  {});

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "SpawnRoads", "LevelTool: Spawn Roads"));

    int32 TotalSegs = 0;
    int32 Skipped   = 0;

    for (const TSharedPtr<FJsonValue>& Val : JsonArray)
    {
        const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
        if (!Obj) continue;

        FString RoadType = Obj->GetStringField(TEXT("type"));
        const TArray<TSharedPtr<FJsonValue>>& PtsJson = Obj->GetArrayField(TEXT("points_ue5"));
        if (PtsJson.Num() < 2) { Skipped++; continue; }

        // Collect Z-snapped points inside landscape bounds
        TArray<FVector> Pts;
        for (const TSharedPtr<FJsonValue>& PtVal : PtsJson)
        {
            const TArray<TSharedPtr<FJsonValue>>& Pt = PtVal->AsArray();
            if (Pt.Num() < 2) continue;
            float X = (float)Pt[0]->AsNumber();
            float Y = (float)Pt[1]->AsNumber();

            if (LandscapeBounds.IsValid &&
                (X < LandscapeBounds.Min.X || X > LandscapeBounds.Max.X ||
                 Y < LandscapeBounds.Min.Y || Y > LandscapeBounds.Max.Y))
                continue;

            // Read terrain height directly from cached heightmap — collision not ready yet.
            const float Z = GetTerrainZAtWorldXY(X, Y) + 3.f;
            Pts.Add(FVector(X, Y, Z));
        }
        if (Pts.Num() < 2) { Skipped++; continue; }

        // Ensure type exists in map (fallback to minor)
        if (!Geoms.Contains(RoadType)) RoadType = TEXT("minor");
        FRoadGeom&  G     = Geoms[RoadType];
        FRoadProps  Props = GetProps(RoadType);

        // Build flat quad geometry per segment
        for (int32 i = 0; i < Pts.Num() - 1; i++)
        {
            const FVector& P0  = Pts[i];
            const FVector& P1  = Pts[i + 1];
            FVector Dir   = (P1 - P0).GetSafeNormal2D();
            FVector Right = FVector(-Dir.Y, Dir.X, 0.f);   // 2D perpendicular

            FVector V0 = P0 - Right * Props.HalfWidthCm;
            FVector V1 = P0 + Right * Props.HalfWidthCm;
            FVector V2 = P1 + Right * Props.HalfWidthCm;
            FVector V3 = P1 - Right * Props.HalfWidthCm;

            float SegM = FVector::Dist(P0, P1) / 100.f;   // cm → m for UV tiling
            int32 Base = G.Vertices.Num();
            G.Vertices.Append({ V0, V1, V2, V3 });
            G.Normals.Append({ FVector::UpVector, FVector::UpVector,
                               FVector::UpVector, FVector::UpVector });
            G.UVs.Append({
                { G.UOffset,        0.f },
                { G.UOffset,        1.f },
                { G.UOffset + SegM, 1.f },
                { G.UOffset + SegM, 0.f }
            });
            G.UOffset += SegM;
            // Two triangles: (0,1,2) and (0,2,3)
            G.Triangles.Append({ Base, Base+1, Base+2, Base, Base+2, Base+3 });
            TotalSegs++;
        }
    }

    // Spawn one actor per road type
    int32 SpawnedActors = 0;
    for (auto& KV : Geoms)
    {
        FRoadGeom& G = KV.Value;
        if (G.Vertices.IsEmpty()) continue;

        FActorSpawnParameters Params;
        Params.bNoFail = true;
        AActor* RoadActor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
        if (!RoadActor) continue;

        UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(RoadActor, TEXT("RoadMesh"));
        PMC->SetupAttachment(nullptr);
        PMC->RegisterComponent();
        RoadActor->SetRootComponent(PMC);
        RoadActor->AddInstanceComponent(PMC);
        PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        TArray<FLinearColor>    Colors;
        TArray<FProcMeshTangent> Tangents;
        PMC->CreateMeshSection_LinearColor(0,
            G.Vertices, G.Triangles, G.Normals, G.UVs, Colors, Tangents, false);

        if (BaseMat)
        {
            FRoadProps Props = GetProps(KV.Key);
            UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, RoadActor);
            Mat->SetVectorParameterValue(TEXT("Color"),
                FVector4(Props.Color.R, Props.Color.G, Props.Color.B, 1.f));
            PMC->SetMaterial(0, Mat);
        }

        RoadActor->SetActorLabel(FString::Printf(TEXT("Roads_%s"), *KV.Key));
        RoadActor->Tags.Add(*KV.Key);
        RoadActor->SetFolderPath(TEXT("LevelTool/Roads"));
        SpawnedActors++;
    }

    Log(FString::Printf(TEXT("  ✔ Roads: %d segments → %d actors  (%d ways skipped)"),
        TotalSegs, SpawnedActors, Skipped));
}

// ─── WATER REMOVED ────────────────────────────────────────────────────────────
// SpawnWaterActors removed. Water plugin dependency eliminated.
// ─────────────────────────────────────────────────────────────────────────────

#if 0
void ULevelToolSubsystem::SpawnWaterActors_REMOVED(const FString& JsonPath, const FBox& LandscapeBounds,
                                            float OceanWorldZCm)
{
    if (!FPaths::FileExists(JsonPath))
    {
        Log(FString::Printf(TEXT("  ⚠ Water JSON not found: %s"), *JsonPath));
        return;
    }

    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return;

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root) return;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "SpawnWater", "LevelTool: Spawn Water"));

    // ── WaterZoneActor check ──────────────────────────────────────────────────
    // AWaterZoneActor (UE5.7) renders the water mesh but uses MinimalAPI, so it
    // cannot be spawned from plugin code. Warn the user if none is in the level.
    {
        UClass* WaterZoneClass = FindObject<UClass>(nullptr, TEXT("/Script/Water.WaterZoneActor"));
        bool bHasWaterZone = false;
        if (WaterZoneClass)
        {
            for (TActorIterator<AActor> It(World, WaterZoneClass); It; ++It)
            {
                bHasWaterZone = true;
                break;
            }
        }
        if (!bHasWaterZone)
        {
            Log(TEXT("  ⚠ No WaterZoneActor found — add one manually to see water:"));
            Log(TEXT("    Place Actors → Water → Water Zone, position at landscape center"));
        }
        else
        {
            Log(TEXT("  ✔ WaterZoneActor found"));
        }
    }

    int32 LakeCount  = 0;
    int32 RiverCount = 0;

    // ── Ocean ────────────────────────────────────────────────────────────────
    bool bHasOcean = Root->GetBoolField(TEXT("has_ocean"));
    if (bHasOcean)
    {
        FVector OceanPos(
            LandscapeBounds.IsValid ? LandscapeBounds.GetCenter().X : 0.f,
            LandscapeBounds.IsValid ? LandscapeBounds.GetCenter().Y : 0.f,
            OceanWorldZCm);

        Log(FString::Printf(TEXT("  Ocean Z: %.0fcm  (sea surface above carved floor)"), OceanWorldZCm));

        FActorSpawnParameters Params;
        Params.bNoFail = true;
        AWaterBodyOcean* Ocean = World->SpawnActor<AWaterBodyOcean>(
            AWaterBodyOcean::StaticClass(), OceanPos, FRotator::ZeroRotator, Params);
        if (Ocean)
        {
            Ocean->SetActorLabel(TEXT("WaterBody_Ocean"));
            Ocean->Tags.AddUnique(TEXT("LevelToolGenerated"));
            Ocean->SetFolderPath(TEXT("LevelTool/Water"));
            Ocean->PostEditChange();
            Log(FString::Printf(TEXT("  ✔ WaterBodyOcean spawned at Z=%.0fcm"), OceanWorldZCm));
        }
        else
        {
            Log(TEXT("  ⚠ WaterBodyOcean spawn failed — check Water Plugin is enabled"));
        }
    }

    // ── Lakes ────────────────────────────────────────────────────────────────
    const TArray<TSharedPtr<FJsonValue>>* LakesArr;
    if (Root->TryGetArrayField(TEXT("lakes"), LakesArr))
    {
        for (const TSharedPtr<FJsonValue>& LakeVal : *LakesArr)
        {
            const TSharedPtr<FJsonObject>* LakeObj;
            if (!LakeVal->TryGetObject(LakeObj)) continue;

            const TArray<TSharedPtr<FJsonValue>>* PtsArr;
            if (!(*LakeObj)->TryGetArrayField(TEXT("points_ue5"), PtsArr)) continue;
            if (PtsArr->Num() < 3) continue;

            FActorSpawnParameters Params;
            Params.bNoFail = true;
            AWaterBodyLake* LakeActor = World->SpawnActor<AWaterBodyLake>(
                AWaterBodyLake::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
            if (!LakeActor) continue;

            // Set spline points via USplineComponent (base of UWaterSplineComponent)
            USplineComponent* Spline = LakeActor->FindComponentByClass<USplineComponent>();
            if (Spline)
            {
                Spline->ClearSplinePoints(false);
                for (const TSharedPtr<FJsonValue>& PtVal : *PtsArr)
                {
                    const TArray<TSharedPtr<FJsonValue>>* Coords;
                    if (!PtVal->TryGetArray(Coords) || Coords->Num() < 2) continue;
                    float X = (float)(*Coords)[0]->AsNumber();
                    float Y = (float)(*Coords)[1]->AsNumber();
                    Spline->AddSplinePoint(FVector(X, Y, 0.f),
                        ESplineCoordinateSpace::World, false);
                }
                Spline->SetClosedLoop(true, false);
                Spline->UpdateSpline();
            }

            LakeActor->SetActorLabel(FString::Printf(TEXT("WaterBody_Lake_%d"), LakeCount));
            LakeActor->Tags.AddUnique(TEXT("LevelToolGenerated"));
            LakeActor->SetFolderPath(TEXT("LevelTool/Water"));
            LakeActor->PostEditChange();
            LakeCount++;
        }
    }

    // ── Rivers ───────────────────────────────────────────────────────────────
    const TArray<TSharedPtr<FJsonValue>>* RiversArr;
    if (Root->TryGetArrayField(TEXT("rivers"), RiversArr))
    {
        for (const TSharedPtr<FJsonValue>& RiverVal : *RiversArr)
        {
            const TSharedPtr<FJsonObject>* RiverObj;
            if (!RiverVal->TryGetObject(RiverObj)) continue;

            const TArray<TSharedPtr<FJsonValue>>* PtsArr;
            if (!(*RiverObj)->TryGetArrayField(TEXT("points_ue5"), PtsArr)) continue;
            if (PtsArr->Num() < 2) continue;

            FActorSpawnParameters Params;
            Params.bNoFail = true;
            AWaterBodyRiver* RiverActor = World->SpawnActor<AWaterBodyRiver>(
                AWaterBodyRiver::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
            if (!RiverActor) continue;

            USplineComponent* Spline = RiverActor->FindComponentByClass<USplineComponent>();
            if (Spline)
            {
                Spline->ClearSplinePoints(false);
                for (const TSharedPtr<FJsonValue>& PtVal : *PtsArr)
                {
                    const TArray<TSharedPtr<FJsonValue>>* Coords;
                    if (!PtVal->TryGetArray(Coords) || Coords->Num() < 2) continue;
                    float X = (float)(*Coords)[0]->AsNumber();
                    float Y = (float)(*Coords)[1]->AsNumber();
                    Spline->AddSplinePoint(FVector(X, Y, 0.f),
                        ESplineCoordinateSpace::World, false);
                }
                Spline->SetClosedLoop(false, false);
                Spline->UpdateSpline();
            }

            RiverActor->SetActorLabel(FString::Printf(TEXT("WaterBody_River_%d"), RiverCount));
            RiverActor->Tags.AddUnique(TEXT("LevelToolGenerated"));
            RiverActor->SetFolderPath(TEXT("LevelTool/Water"));
            RiverActor->PostEditChange();
            RiverCount++;
        }
    }

    Log(FString::Printf(TEXT("  ✔ Water: ocean=%s  lakes=%d  rivers=%d"),
        bHasOcean ? TEXT("yes") : TEXT("no"), LakeCount, RiverCount));
}
#endif  // water removed

// ─────────────────────────────────────────────────────────────────────────────
//  Splat Map Import
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::ImportSplatMapsAsTextures(const FString& HeightmapPngPath,
                                                     ALandscape* Landscape)
{
    // Splat PNGs are in the same folder as the heightmap, named {prefix}_splat_{layer}.png
    const FString Dir    = FPaths::GetPath(HeightmapPngPath);
    FString       Prefix = FPaths::GetBaseFilename(HeightmapPngPath)
                               .Replace(TEXT("_heightmap"), TEXT(""));

    const TArray<FString> Layers = { TEXT("grass"), TEXT("rock"), TEXT("sand"), TEXT("snow") };

    TArray<FString> FilesToImport;
    for (const FString& Layer : Layers)
    {
        FString SplatPath = FPaths::Combine(Dir,
            FString::Printf(TEXT("%s_splat_%s.png"), *Prefix, *Layer));
        if (FPaths::FileExists(SplatPath))
            FilesToImport.Add(SplatPath);
    }

    if (FilesToImport.IsEmpty())
    {
        Log(TEXT("  ℹ No splat maps found — skipping texture import"));
        return;
    }

    Log(FString::Printf(TEXT("  ℹ Splat maps found (%d) — importing on next frame..."), FilesToImport.Num()));

    // ImportAssetsAutomated uses the Interchange pipeline which dispatches its own
    // TaskGraph tasks. Calling it directly from inside a TaskGraphMainThread callback
    // hits the RecursionGuard assertion. Defer to the next engine tick instead.
    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, FilesToImport](float) -> bool
        {
            if (ULevelToolSubsystem* Self = WeakThis.Get())
            {
                IAssetTools& AssetTools =
                    FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

                UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
                ImportData->DestinationPath  = TEXT("/Game/LevelTool/SplatMaps");
                ImportData->Filenames        = FilesToImport;
                ImportData->bReplaceExisting = true;

                TArray<UObject*> Imported = AssetTools.ImportAssetsAutomated(ImportData);

                Self->Log(FString::Printf(
                    TEXT("  ✔ Splat maps imported: %d textures → /Game/LevelTool/SplatMaps/"),
                    Imported.Num()));
                Self->Log(TEXT("    → Assign to Landscape Material layer blend nodes (grass/rock/sand/snow)"));
            }
            return false; // run once
        })
    );
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compass Markers
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::SpawnCompassMarkers(UWorld* World, const FBox& LandscapeBounds)
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UMaterial*   BaseMat  = LoadObject<UMaterial>(nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!CubeMesh || !BaseMat)
    {
        Log(TEXT("  ⚠ Compass: could not load BasicShapes assets"));
        return;
    }

    const FString Folder = TEXT("LevelTool/Compass");

    // Pillar height = 20% of the landscape diagonal, min 5000 cm
    float DiagCm     = (LandscapeBounds.Max - LandscapeBounds.Min).Size2D();
    float PillarH    = FMath::Max(DiagCm * 0.20f, 5000.0f);
    float PillarW    = FMath::Max(DiagCm * 0.008f, 200.0f);  // width ~ 0.8% of diagonal
    float BaseZ      = LandscapeBounds.Max.Z;                 // sit on top of terrain

    FVector Center2D = FVector(
        (LandscapeBounds.Min.X + LandscapeBounds.Max.X) * 0.5f,
        (LandscapeBounds.Min.Y + LandscapeBounds.Max.Y) * 0.5f,
        0.f);

    // Helper: spawn one colored cube pillar
    auto SpawnPillar = [&](FVector XY_Pos, FLinearColor Color, const FString& Label)
    {
        // CubeMesh is 100cm cube at scale 1 — pivot at center
        FVector SpawnLoc(XY_Pos.X, XY_Pos.Y, BaseZ + PillarH * 0.5f);

        FActorSpawnParameters P;
        P.bNoFail = true;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), SpawnLoc, FRotator::ZeroRotator, P);
        if (!Actor) return;

        UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
        Comp->SetStaticMesh(CubeMesh);
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, Actor);
        Mat->SetVectorParameterValue(TEXT("Color"), FVector4(Color.R, Color.G, Color.B, 1.0f));
        Comp->SetMaterial(0, Mat);

        // Scale: XY = pillar width, Z = pillar height (cube is 100cm → divide by 100)
        Actor->SetActorScale3D(FVector(PillarW / 100.f, PillarW / 100.f, PillarH / 100.f));
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(*Folder);
    };

    float EdgeOffset = FMath::Min(
        (LandscapeBounds.Max.X - LandscapeBounds.Min.X) * 0.45f,
        (LandscapeBounds.Max.Y - LandscapeBounds.Min.Y) * 0.45f);

    // Center — RED (입력 중심 좌표)
    SpawnPillar(Center2D, FLinearColor(1.f, 0.f, 0.f), TEXT("Compass_CENTER"));

    // North (Y-) — BLUE
    SpawnPillar(Center2D + FVector(0, -EdgeOffset, 0), FLinearColor(0.f, 0.4f, 1.f), TEXT("Compass_N"));

    // South (Y+) — YELLOW
    SpawnPillar(Center2D + FVector(0, +EdgeOffset, 0), FLinearColor(1.f, 0.9f, 0.f), TEXT("Compass_S"));

    // East (X+) — GREEN
    SpawnPillar(Center2D + FVector(+EdgeOffset, 0, 0), FLinearColor(0.f, 0.9f, 0.f), TEXT("Compass_E"));

    // West (X-) — ORANGE
    SpawnPillar(Center2D + FVector(-EdgeOffset, 0, 0), FLinearColor(1.f, 0.5f, 0.f), TEXT("Compass_W"));

    Log(TEXT("  ✔ Compass markers spawned (LevelTool/Compass)"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Building Placement
// ─────────────────────────────────────────────────────────────────────────────

bool ULevelToolSubsystem::LoadBuildingsJson(
    const FString& JsonPath, TArray<FBuildingEntry>& OutBuildings)
{
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath))
    {
        Log(FString::Printf(TEXT("✖ Cannot read buildings JSON: %s"), *JsonPath));
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> JsonArray;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, JsonArray))
    {
        Log(TEXT("✖ Failed to parse buildings JSON"));
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Val : JsonArray)
    {
        const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
        if (!Obj) continue;

        FBuildingEntry Entry;
        Entry.OsmId    = (int64)Obj->GetNumberField(TEXT("id"));
        Entry.TypeKey  = Obj->GetStringField(TEXT("type"));
        Entry.HeightM  = (float)Obj->GetNumberField(TEXT("height_m"));
        Entry.AreaM2   = (float)Obj->GetNumberField(TEXT("area_m2"));

        const TArray<TSharedPtr<FJsonValue>>& Centroid =
            Obj->GetArrayField(TEXT("centroid_ue5"));
        if (Centroid.Num() >= 2)
        {
            Entry.CentroidUE5.X = (float)Centroid[0]->AsNumber();
            Entry.CentroidUE5.Y = (float)Centroid[1]->AsNumber();
        }

        OutBuildings.Add(Entry);
    }

    return true;
}

int32 ULevelToolSubsystem::PlaceBuildingsFromJson(
    const FString& JsonPath, ULevelToolBuildingPool* Pool, float ZOffsetCm)
{
    TArray<FBuildingEntry> Buildings;
    if (!LoadBuildingsJson(JsonPath, Buildings))
        return 0;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Log(TEXT("✖ No editor world"));
        return 0;
    }

    // Find Landscape actor for XY bounds filtering
    ALandscape* LandscapeActor = nullptr;
    FBox LandscapeBounds(ForceInit);
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        LandscapeActor = *It;
        LandscapeBounds = It->GetComponentsBoundingBox(true);
        break;
    }
    const bool bHaveLandscape = LandscapeActor != nullptr && LandscapeBounds.IsValid;
    if (bHaveLandscape)
    {
        Log(FString::Printf(TEXT("  Landscape bounds XY: [%.0f,%.0f] – [%.0f,%.0f] cm"),
            LandscapeBounds.Min.X, LandscapeBounds.Min.Y,
            LandscapeBounds.Max.X, LandscapeBounds.Max.Y));
    }

    FScopedTransaction Transaction(
        NSLOCTEXT("LevelTool", "PlaceBuildings", "LevelTool: Place Buildings"));

    // Group log: counts per type
    TMap<FString, int32> TypeCounts;
    int32 Placed   = 0;
    int32 Skipped  = 0;
    int32 OutOfBounds = 0;

    // Create a folder in the World Outliner
    const FString FolderPath = TEXT("LevelTool/Buildings");

    for (const FBuildingEntry& Building : Buildings)
    {
        // 1. Skip buildings outside Landscape XY bounds
        if (bHaveLandscape)
        {
            const float BX = Building.CentroidUE5.X;
            const float BY = Building.CentroidUE5.Y;
            if (BX < LandscapeBounds.Min.X || BX > LandscapeBounds.Max.X ||
                BY < LandscapeBounds.Min.Y || BY > LandscapeBounds.Max.Y)
            {
                OutOfBounds++;
                continue;
            }
        }

        // 2. Z snap: read terrain height directly from cached heightmap data.
        // (Landscape collision is cooked async after spawn, so line traces at this
        //  point would miss the terrain and return Z=0.)
        const float GroundZ = GetTerrainZAtWorldXY(Building.CentroidUE5.X, Building.CentroidUE5.Y) + ZOffsetCm;

        AStaticMeshActor* Actor = SpawnBuildingActor(Building, Pool, GroundZ);
        if (Actor)
        {
            Actor->SetFolderPath(*FolderPath);
            Placed++;
            TypeCounts.FindOrAdd(Building.TypeKey)++;
        }
        else
        {
            Skipped++;
        }
    }

    if (OutOfBounds > 0)
        Log(FString::Printf(TEXT("  ⚠ Skipped %d buildings outside Landscape bounds"), OutOfBounds));

    // Summary log
    for (auto& Pair : TypeCounts)
    {
        Log(FString::Printf(TEXT("    %4d × %s"), Pair.Value, *Pair.Key));
    }
    if (Skipped > 0)
    {
        Log(FString::Printf(TEXT("  ⚠ Skipped %d (no mesh in pool)"), Skipped));
    }

    return Placed;
}

AStaticMeshActor* ULevelToolSubsystem::SpawnBuildingActor(
    const FBuildingEntry& Building, ULevelToolBuildingPool* Pool, float GroundZ)
{
    UStaticMesh* Mesh = Pool ? Pool->ResolveMesh(Building.TypeKey) : nullptr;
    if (!Mesh) return nullptr;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    FActorSpawnParameters Params;
    Params.Name         = FName(*FString::Printf(TEXT("Building_%lld"), Building.OsmId));
    Params.NameMode     = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    Params.bNoFail      = false;

    // Compute scale before spawning so we can offset Z correctly
    FVector Scale = ComputeBuildingScale(Building, Mesh);

    // Place the actor so the mesh BOTTOM sits exactly on GroundZ.
    // Formula: SpawnZ = GroundZ + (BoxExtent.Z - Origin.Z) * Scale.Z
    //   - Pivot at center: Origin.Z≈0, BoxExtent.Z = HalfHeight  → offset = HalfHeight * Scale.Z  ✓
    //   - Pivot at bottom: Origin.Z≈BoxExtent.Z                   → offset ≈ 0                     ✓
    FBoxSphereBounds Bounds = Mesh->GetBounds();
    float BottomOffsetCm    = (Bounds.BoxExtent.Z - Bounds.Origin.Z) * Scale.Z;
    FVector Location(Building.CentroidUE5.X, Building.CentroidUE5.Y, GroundZ + BottomOffsetCm);

    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, Params);

    if (!Actor) return nullptr;

    UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
    Comp->SetStaticMesh(Mesh);
    Comp->SetCollisionProfileName(TEXT("BlockAll"));

    Actor->SetActorRelativeScale3D(Scale);

    // Label
    Actor->SetActorLabel(FString::Printf(TEXT("Building_%lld"), Building.OsmId));

    // Tags for gameplay/PCG
    Actor->Tags.Add(*Building.TypeKey);
    Actor->Tags.Add(*FString::Printf(TEXT("h_%dm"), FMath::RoundToInt(Building.HeightM)));
    Actor->Tags.Add(*FString::Printf(TEXT("osm_%lld"), Building.OsmId));

    // Apply materials: entry-specific → pool default → mesh default (in priority order)
    if (Pool)
    {
        UMaterialInterface* WallMat = nullptr;
        UMaterialInterface* RoofMat = nullptr;

        // 1. Entry-specific overrides
        if (const FBuildingMeshEntry* Entry = Pool->FindEntry(Building.TypeKey))
        {
            WallMat = Entry->WallMaterial.LoadSynchronous();
            RoofMat = Entry->RoofMaterial.LoadSynchronous();
        }

        // 2. Fall back to pool-wide defaults
        if (!WallMat) WallMat = Pool->DefaultWallMaterial.LoadSynchronous();
        if (!RoofMat) RoofMat = Pool->DefaultRoofMaterial.LoadSynchronous();

        // 3. Apply whatever we found
        if (WallMat) Comp->SetMaterial(0, WallMat);
        if (RoofMat) Comp->SetMaterial(1, RoofMat);
    }

    return Actor;
}

FVector ULevelToolSubsystem::ComputeBuildingScale(
    const FBuildingEntry& Building, UStaticMesh* Mesh) const
{
    // Base assumption: mesh is 100cm on each axis at scale 1.0
    float MeshHeightCm = 100.0f;
    float MeshXCm      = 100.0f;
    float MeshYCm      = 100.0f;

    // Read actual mesh bounds for accurate per-axis scaling
    if (Mesh)
    {
        FBoxSphereBounds Bounds = Mesh->GetBounds();
        float AH = Bounds.BoxExtent.Z * 2.0f;
        float AX = Bounds.BoxExtent.X * 2.0f;
        float AY = Bounds.BoxExtent.Y * 2.0f;
        if (AH > 1.0f) MeshHeightCm = AH;
        if (AX > 1.0f) MeshXCm      = AX;
        if (AY > 1.0f) MeshYCm      = AY;
    }

    // Z: match OSM height with small random variation
    float TargetHeightCm = Building.HeightM * 100.0f;
    float ZScale         = (TargetHeightCm / MeshHeightCm) * FMath::RandRange(0.9f, 1.1f);

    // XY: match footprint area — treat as a square whose side = sqrt(area)
    float SideCm = FMath::Sqrt(FMath::Max(Building.AreaM2, 1.0f)) * 100.0f;
    float XScale = SideCm / MeshXCm;
    float YScale = SideCm / MeshYCm;

    return FVector(XScale, YScale, FMath::Max(ZScale, 0.1f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Utility
// ─────────────────────────────────────────────────────────────────────────────

bool ULevelToolSubsystem::ValidateSettings(TArray<FString>& OutErrors) const
{
    const ULevelToolSettings* S = ULevelToolSettings::Get();

    if (S->PythonScriptDir.Path.IsEmpty())
        OutErrors.Add(TEXT("Python Script Directory is not set"));
    else if (!FPaths::DirectoryExists(S->PythonScriptDir.Path))
        OutErrors.Add(FString::Printf(TEXT("Python Script Directory not found: %s"), *S->PythonScriptDir.Path));

    FString MainPy = GetPythonScriptPath(TEXT("main.py"));
    if (!FPaths::FileExists(MainPy))
        OutErrors.Add(FString::Printf(TEXT("main.py not found at: %s"), *MainPy));

    if (S->ElevationSource == EElevationSource::GoogleMaps && S->GoogleMapsApiKey.IsEmpty())
        OutErrors.Add(TEXT("Google Maps API Key is required for the selected elevation source"));

    if (S->OutputDir.Path.IsEmpty())
        OutErrors.Add(TEXT("Output Directory is not set"));

    return OutErrors.IsEmpty();
}

float ULevelToolSubsystem::GetTerrainZAtWorldXY(float WorldX, float WorldY) const
{
    // Landscape pixel (0,0) maps to world (0,0). Actor placed at (0,0,ActorZ).
    // WorldZ = ZScale * H / 128.0  (where ActorZ = 32768*ZScale/128 folds in)
    if (CachedHeightData.IsEmpty() || CachedHMapWidth <= 0 || CachedXYScaleCm <= 0.f)
        return 0.f;

    // Landscape origin (actor position) = top-left corner of the heightmap.
    // Subtract it to get pixel-local coordinates.
    const int32 PixX = FMath::Clamp(FMath::RoundToInt32((WorldX - CachedOriginX) / CachedXYScaleCm), 0, CachedHMapWidth - 1);
    const int32 PixY = FMath::Clamp(FMath::RoundToInt32((WorldY - CachedOriginY) / CachedXYScaleCm), 0, CachedHMapWidth - 1);
    const uint16 H   = CachedHeightData[PixY * CachedHMapWidth + PixX];
    return CachedZScale * static_cast<float>(H) / 128.0f;
}

void ULevelToolSubsystem::Log(const FString& Msg)
{
    LogLines.Add(Msg);
    UE_LOG(LogLevelTool, Log, TEXT("%s"), *Msg);

    if (IsInGameThread())
    {
        OnLog.Broadcast(Msg);
    }
    else
    {
        Async(EAsyncExecution::TaskGraphMainThread, [this, Msg]()
        {
            OnLog.Broadcast(Msg);
        });
    }
}

void ULevelToolSubsystem::SetProgress(const FString& Stage, float Pct)
{
    Log(FString::Printf(TEXT("[%.0f%%] %s"), Pct * 100.f, *Stage));

    if (IsInGameThread())
    {
        OnProgress.Broadcast(Stage, Pct);
    }
    else
    {
        Async(EAsyncExecution::TaskGraphMainThread, [this, Stage, Pct]()
        {
            OnProgress.Broadcast(Stage, Pct);
        });
    }
}

void ULevelToolSubsystem::FinishJob(bool bSuccess)
{
    bJobRunning = false;

    if (IsInGameThread())
    {
        OnComplete.Broadcast(bSuccess);
    }
    else
    {
        Async(EAsyncExecution::TaskGraphMainThread, [this, bSuccess]()
        {
            OnComplete.Broadcast(bSuccess);
        });
    }
}
