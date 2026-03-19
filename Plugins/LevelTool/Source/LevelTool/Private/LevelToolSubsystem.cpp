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

// Text render for compass markers
#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"

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
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ShaderCompiler.h"
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
    const uint32 Gen = ++JobGeneration;
    LogLines.Empty();
    LastResult = FLevelToolJobResult();

    Log(FString::Printf(TEXT("▶ Starting full pipeline  preset='%s'  lat=%.4f  lon=%.4f  r=%.1fkm"),
        *Preset, Lat, Lon, RadiusKm));

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ULevelToolBuildingPool> WeakPool(Pool);

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool, bSpawnRoads, Gen]()
    {
        ULevelToolSubsystem* Self = WeakThis.Get();
        if (!Self) return;

        FLevelToolJobResult Result;

        // ── Step 1: Run Python main.py all ──────────────────────────────
        Self->SetProgress(TEXT("Fetching terrain and building data..."), 0.05f);

        FString Args     = Self->BuildMainPyArgs(TEXT("all"), Preset, Lat, Lon, RadiusKm);
        FString MainPy   = Self->GetPythonScriptPath(TEXT("main.py"));
        FString Stdout, Stderr;

        // Compute effective XY scale the same way BuildMainPyArgs does
        {
            const ULevelToolSettings* S2 = ULevelToolSettings::Get();
            float DiamCm = RadiusKm * 2.0f * 100000.0f;
            int32 Req = FMath::CeilToInt(DiamCm / S2->XYScaleCm);
            bool bFit = false;
            static const int32 VS[] = { 505, 1009, 2017, 4033 };
            for (int32 V : VS) { if ((V-1) >= Req) { bFit = true; break; } }
            Result.EffectiveXYScaleCm = bFit ? S2->XYScaleCm
                : FMath::CeilToFloat(DiamCm / 4032.f);
        }

        if (!Self->RunPythonScript(MainPy, Args, Stdout, Stderr))
        {
            Result.bSuccess     = false;
            Result.ErrorMessage = FString::Printf(TEXT("Python error:\n%s"), *Stderr);
            Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Result, Gen]()
            {
                ULevelToolSubsystem* S = WeakThis.Get();
                if (!S || S->JobGeneration != Gen) return;
                S->LastResult = Result;
                S->Log(TEXT("✖ Python pipeline failed: ") + Result.ErrorMessage);
                S->FinishJob(false);
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
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, WeakPool, Result, bSpawnRoads, Gen]() mutable
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            if (!Self || Self->JobGeneration != Gen) return;

            Self->SetProgress(TEXT("Importing landscape..."), 0.50f);

            if (!Result.HeightmapPngPath.IsEmpty())
            {
                FString LandscapeName = FPaths::GetBaseFilename(Result.HeightmapPngPath)
                                          .Replace(TEXT("_heightmap"), TEXT(""));
                float ElevRange = (Result.ElevationMaxM > Result.ElevationMinM)
                    ? (Result.ElevationMaxM - Result.ElevationMinM) : 0.f;
                if (!Self->ImportHeightmapAsLandscape(Result.HeightmapPngPath, LandscapeName, ElevRange, Result.EffectiveXYScaleCm))
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

            // Focus viewport on generated landscape
            if (PostWorld)
            {
                for (TActorIterator<ALandscape> FocusIt(PostWorld); FocusIt; ++FocusIt)
                {
                    GEditor->MoveViewportCamerasToActor(**FocusIt, true);
                    break;
                }
            }

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
    const uint32 Gen = ++JobGeneration;
    LogLines.Empty();
    LastResult = FLevelToolJobResult();

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ULevelToolBuildingPool> WeakPool(Pool);

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool, bSpawnRoads, Gen]()
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
            Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Result, Gen]() mutable
            {
                ULevelToolSubsystem* S = WeakThis.Get();
                if (!S || S->JobGeneration != Gen) return;
                S->LastResult = Result;
                S->FinishJob(false);
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

        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, WeakPool, Result, bSpawnRoads, Gen]() mutable
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            if (!Self || Self->JobGeneration != Gen) return;

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
        ++JobGeneration;
        bJobRunning = false;

        {
            FScopeLock Lock(&PythonProcessLock);
            if (ActivePythonProcess.IsValid())
            {
                FPlatformProcess::TerminateProc(ActivePythonProcess, true);
                ActivePythonProcess.Reset();
            }
        }

        Log(TEXT("⚠ Job cancelled — process killed, callbacks invalidated"));
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

    FString PythonExe;
#if PLATFORM_WINDOWS
    {
        FString EnginePython = FPaths::ConvertRelativePathToFull(
            FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
        PythonExe = FPaths::FileExists(EnginePython) ? EnginePython : TEXT("python");
    }
#else
    PythonExe = TEXT("python3");
#endif

    FString ScriptDir = FPaths::GetPath(ScriptPath);
    FString CommandLine = FString::Printf(TEXT("-X utf8 -u \"%s\" %s"), *ScriptPath, *Args);

    Log(FString::Printf(TEXT("$ %s %s"), *PythonExe, *CommandLine));

    // Create pipe for stdout capture (stderr merged in Python via sys.stderr = sys.stdout)
    void* ReadPipe  = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    FProcHandle Proc = FPlatformProcess::CreateProc(
        *PythonExe, *CommandLine, false, true, true,
        nullptr, 0, *ScriptDir, WritePipe);

    if (!Proc.IsValid())
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        OutStderr = TEXT("Failed to launch Python process");
        Log(TEXT("✖ ") + OutStderr);
        return false;
    }

    {
        FScopeLock Lock(&PythonProcessLock);
        ActivePythonProcess = Proc;
    }

    const double StartTime   = FPlatformTime::Seconds();
    const uint32 ExpectedGen = JobGeneration.load();
    FString Stdout;

    while (FPlatformProcess::IsProcRunning(Proc))
    {
        FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
        if (!Chunk.IsEmpty())
        {
            Stdout += Chunk;
            TArray<FString> ChunkLines;
            Chunk.ParseIntoArrayLines(ChunkLines);
            for (const FString& Line : ChunkLines)
            {
                if (!Line.IsEmpty())
                    Log(Line);
            }
        }

        if (FPlatformTime::Seconds() - StartTime > PythonTimeoutSeconds)
        {
            FPlatformProcess::TerminateProc(Proc, true);
            FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
            {
                FScopeLock Lock(&PythonProcessLock);
                ActivePythonProcess.Reset();
            }
            OutStderr = FString::Printf(TEXT("Python process timed out after %.0fs"), PythonTimeoutSeconds);
            Log(TEXT("✖ ") + OutStderr);
            return false;
        }

        if (JobGeneration.load() != ExpectedGen)
        {
            FPlatformProcess::TerminateProc(Proc, true);
            FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
            {
                FScopeLock Lock(&PythonProcessLock);
                ActivePythonProcess.Reset();
            }
            OutStderr = TEXT("Job cancelled");
            return false;
        }

        FPlatformProcess::Sleep(0.1f);
    }

    // Read remaining output after process exit
    FString Remaining = FPlatformProcess::ReadPipe(ReadPipe);
    if (!Remaining.IsEmpty())
    {
        Stdout += Remaining;
        TArray<FString> RemLines;
        Remaining.ParseIntoArrayLines(RemLines);
        for (const FString& Line : RemLines)
        {
            if (!Line.IsEmpty())
                Log(Line);
        }
    }

    int32 ReturnCode = 0;
    FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

    {
        FScopeLock Lock(&PythonProcessLock);
        ActivePythonProcess.Reset();
    }

    OutStdout = Stdout;
    OutStderr = FString();

    if (ReturnCode != 0)
    {
        Log(FString::Printf(TEXT("✖ Python exited with code %d"), ReturnCode));
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

    // Compute the grid size and XY scale needed to cover the requested radius.
    // Strategy: try configured XYScaleCm first, then auto-increase scale if the
    // radius doesn't fit in the largest valid grid (4033).
    const float DiameterCm = RadiusKm * 2.0f * 100000.0f;
    float EffectiveXYScale = S->XYScaleCm;

    static const int32 ValidSizes[] = { 505, 1009, 2017, 4033 };
    int32 EffectiveGridSize = S->HeightmapSize;

    // First pass: find smallest grid that fits at configured XY scale
    int32 RequiredQuads = FMath::CeilToInt(DiameterCm / EffectiveXYScale);
    bool bFits = false;
    for (int32 VS : ValidSizes)
    {
        if ((VS - 1) >= RequiredQuads)
        {
            EffectiveGridSize = VS;
            bFits = true;
            break;
        }
    }

    if (!bFits)
    {
        // Radius too large for max grid at current XY scale.
        // Auto-increase XY scale so 4033 grid covers the full radius.
        EffectiveGridSize = 4033;
        EffectiveXYScale = DiameterCm / (4033.f - 1.f);
        EffectiveXYScale = FMath::CeilToFloat(EffectiveXYScale);  // round up to integer cm
        UE_LOG(LogLevelTool, Log,
            TEXT("XY scale auto-adjusted: %.0f -> %.0f cm/quad  (to cover %.1fkm radius in 4033 grid)"),
            S->XYScaleCm, EffectiveXYScale, RadiusKm);
    }

    if (EffectiveGridSize != S->HeightmapSize)
    {
        UE_LOG(LogLevelTool, Log, TEXT("Grid auto-adjusted: %d -> %d  (to cover %.2fkm radius at %.0f cm/quad)"),
            S->HeightmapSize, EffectiveGridSize, RadiusKm, EffectiveXYScale);
    }

    Args += FString::Printf(TEXT(" --lat %.6f --lon %.6f --radius %.4f"), Lat, Lon, RadiusKm);
    Args += FString::Printf(TEXT(" --grid-size %d"), EffectiveGridSize);
    if (!Preset.IsEmpty())
        Args += FString::Printf(TEXT(" --name \"%s\""), *Preset);

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

bool ULevelToolSubsystem::ParseJsonResultBlock(
    const FString& Output, FLevelToolJobResult& OutResult) const
{
    static const FString Marker = TEXT("__LEVELTOOL_RESULT__:");

    // Find the LAST occurrence (cmd_all outputs multiple markers)
    int32 Idx = Output.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Idx == INDEX_NONE) return false;

    FString JsonStr = Output.Mid(Idx + Marker.Len()).TrimStartAndEnd();
    int32 NewlineIdx;
    if (JsonStr.FindChar(TEXT('\n'), NewlineIdx))
        JsonStr = JsonStr.Left(NewlineIdx).TrimEnd();

    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        return false;

    JsonObj->TryGetStringField(TEXT("heightmap_png"), OutResult.HeightmapPngPath);
    JsonObj->TryGetStringField(TEXT("buildings_json"), OutResult.BuildingsJsonPath);
    JsonObj->TryGetStringField(TEXT("roads_json"), OutResult.RoadsJsonPath);

    double TempMin = 0.0, TempMax = 0.0;
    if (JsonObj->TryGetNumberField(TEXT("elevation_min_m"), TempMin))
        OutResult.ElevationMinM = static_cast<float>(TempMin);
    if (JsonObj->TryGetNumberField(TEXT("elevation_max_m"), TempMax))
        OutResult.ElevationMaxM = static_cast<float>(TempMax);

    OutResult.bSuccess = !OutResult.HeightmapPngPath.IsEmpty() ||
                         !OutResult.BuildingsJsonPath.IsEmpty();

    if (OutResult.bSuccess)
        UE_LOG(LogLevelTool, Log, TEXT("ParseJsonResult: parsed successfully from structured JSON block"));

    return OutResult.bSuccess;
}

void ULevelToolSubsystem::ParsePythonOutput(
    const FString& Stdout, FLevelToolJobResult& OutResult) const
{
    // Prefer structured JSON result block
    if (ParseJsonResultBlock(Stdout, OutResult))
        return;

    // Fall back to legacy text pattern matching
    const ULevelToolSettings* S = ULevelToolSettings::Get();

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
    const FString& HeightmapPngPath, const FString& LandscapeName,
    float ElevationRangeM, float OverrideXYScaleCm)
{
    if (!FPaths::FileExists(HeightmapPngPath))
    {
        Log(FString::Printf(TEXT("✖ Heightmap not found: %s"), *HeightmapPngPath));
        return false;
    }

    const ULevelToolSettings* S = ULevelToolSettings::Get();
    const float XYScale = (OverrideXYScaleCm > 0.f) ? OverrideXYScaleCm : S->XYScaleCm;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Log(TEXT("✖ No editor world available"));
        return false;
    }

    // Detect actual heightmap size from the R16 file (fileBytes = W * H * 2).
    // This allows the grid to auto-adjust when BuildMainPyArgs picks a larger size for the radius.
    int32 DetectedSize = S->HeightmapSize;
    {
        FString R16Check = FPaths::ChangeExtension(HeightmapPngPath, TEXT("r16"));
        if (FPaths::FileExists(R16Check))
        {
            int64 FileBytes = IFileManager::Get().FileSize(*R16Check);
            if (FileBytes > 0)
            {
                int32 NumPixels = static_cast<int32>(FileBytes / 2);
                int32 Side = FMath::RoundToInt32(FMath::Sqrt(static_cast<float>(NumPixels)));
                if (Side * Side == NumPixels && Side > 0)
                {
                    DetectedSize = Side;
                    if (DetectedSize != S->HeightmapSize)
                    {
                        Log(FString::Printf(TEXT("  ℹ Heightmap size detected from R16: %d×%d (settings: %d)"),
                            DetectedSize, DetectedSize, S->HeightmapSize));
                    }
                }
            }
        }
    }

    const int32 SectionSize       = 63;    // quads per section
    const int32 SectionsPerComp   = 1;
    const int32 QuadsPerComp      = SectionSize * SectionsPerComp;
    const int32 TargetSize        = DetectedSize - 1;  // vertices → quads
    const int32 ComponentCountX   = FMath::CeilToInt((float)TargetSize / QuadsPerComp);
    const int32 ComponentCountY   = ComponentCountX;

    // Prefer .r16 raw binary (little-endian uint16) — more reliable than PNG 16-bit decoding
    const int32 NumVerts = DetectedSize * DetectedSize;
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
    // Landscape total width = (HMapWidth-1) * XYScale, so half = offset below.
    const float HalfSizeCm = (DetectedSize - 1) * XYScale / 2.0f;
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

    Landscape->SetActorRelativeScale3D(FVector(XYScale, XYScale, ZScale));
    Landscape->SetActorLabel(LandscapeName);
    Landscape->Tags.AddUnique(TEXT("LevelToolGenerated"));

    Log(FString::Printf(TEXT("  ElevationRangeM: %.2fm  ZScale: %.4f  ActorXY: (%.0f, %.0f)  ActorZ: %.0fcm"),
        ActualRangeM, ZScale, ActorX, ActorY, ActorZ));

    // Keep a copy for direct HeightmapTexture write (see below).
    TArray<uint16> HeightDataCopy = HeightData;
    const int32    HMapWidth      = DetectedSize;

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
    CachedXYScaleCm   = XYScale;
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
        XYScale, XYScale, ZScale,
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
    struct FRoadProps { float HalfWidthCm; FLinearColor Color; float ZPriorityCm; };
    auto GetProps = [](const FString& Type) -> FRoadProps
    {
        if (Type == TEXT("major")) return { 600.f, FLinearColor(0.08f, 0.08f, 0.08f), 6.f };
        if (Type == TEXT("path"))  return { 100.f, FLinearColor(0.55f, 0.50f, 0.38f), 0.f };
        return                            { 300.f, FLinearColor(0.18f, 0.18f, 0.18f), 3.f };
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
        float RoadWidthM = (float)Obj->GetNumberField(TEXT("width_m"));
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

            const float Z = GetTerrainZAtWorldXY(X, Y) + 3.f;
            Pts.Add(FVector(X, Y, Z));
        }
        if (Pts.Num() < 2) { Skipped++; continue; }

        // R-A: Subdivide long segments — insert vertex every 5m for terrain conformance
        {
            constexpr float MaxSegLenCm = 500.f;
            TArray<FVector> Sub;
            Sub.Reserve(Pts.Num() * 4);
            Sub.Add(Pts[0]);
            for (int32 j = 1; j < Pts.Num(); j++)
            {
                float Dist2D = FVector::Dist2D(Pts[j-1], Pts[j]);
                int32 NumSubs = FMath::Max(1, FMath::CeilToInt32(Dist2D / MaxSegLenCm));
                for (int32 s = 1; s <= NumSubs; s++)
                {
                    float A  = static_cast<float>(s) / static_cast<float>(NumSubs);
                    float IX = FMath::Lerp(Pts[j-1].X, Pts[j].X, A);
                    float IY = FMath::Lerp(Pts[j-1].Y, Pts[j].Y, A);
                    float IZ = GetTerrainZAtWorldXY(IX, IY) + 3.f;
                    Sub.Add(FVector(IX, IY, IZ));
                }
            }
            Pts = MoveTemp(Sub);
        }

        // Light Z smoothing on the now-dense polyline (radius 1 = 3-point average)
        {
            TArray<float> SmoothedZ;
            SmoothedZ.SetNumUninitialized(Pts.Num());
            for (int32 j = 0; j < Pts.Num(); j++)
            {
                float Sum   = 0.f;
                int32 Count = 0;
                for (int32 k = FMath::Max(0, j - 1);
                     k <= FMath::Min(Pts.Num() - 1, j + 1); k++)
                {
                    Sum += Pts[k].Z;
                    Count++;
                }
                SmoothedZ[j] = Sum / Count;
            }
            for (int32 j = 0; j < Pts.Num(); j++)
            {
                Pts[j].Z = SmoothedZ[j];
            }
        }

        if (!Geoms.Contains(RoadType)) RoadType = TEXT("minor");
        FRoadGeom&  G     = Geoms[RoadType];
        FRoadProps  Props = GetProps(RoadType);

        // Override half-width from OSM width_m if present
        if (RoadWidthM > 0.f)
        {
            Props.HalfWidthCm = RoadWidthM * 50.f;
        }

        // R-B: Build terrain-conforming quad geometry per segment
        const float ZOff = 3.f + Props.ZPriorityCm;
        for (int32 i = 0; i < Pts.Num() - 1; i++)
        {
            const FVector& P0  = Pts[i];
            const FVector& P1  = Pts[i + 1];
            FVector Dir   = (P1 - P0).GetSafeNormal2D();
            FVector Right = FVector(-Dir.Y, Dir.X, 0.f);

            FVector V0 = P0 - Right * Props.HalfWidthCm;
            FVector V1 = P0 + Right * Props.HalfWidthCm;
            FVector V2 = P1 + Right * Props.HalfWidthCm;
            FVector V3 = P1 - Right * Props.HalfWidthCm;

            // Snap each edge vertex Z to terrain independently (cross-slope conformance)
            V0.Z = GetTerrainZAtWorldXY(V0.X, V0.Y) + ZOff;
            V1.Z = GetTerrainZAtWorldXY(V1.X, V1.Y) + ZOff;
            V2.Z = GetTerrainZAtWorldXY(V2.X, V2.Y) + ZOff;
            V3.Z = GetTerrainZAtWorldXY(V3.X, V3.Y) + ZOff;

            // Compute face normal from actual geometry instead of flat UpVector
            FVector FaceN = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
            if (FaceN.Z < 0.f) FaceN = -FaceN;

            float SegM = FVector::Dist(P0, P1) / 100.f;
            int32 Base = G.Vertices.Num();
            G.Vertices.Append({ V0, V1, V2, V3 });
            G.Normals.Append({ FaceN, FaceN, FaceN, FaceN });
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

// ─────────────────────────────────────────────────────────────────────────────
//  Splat Map Import
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::ImportSplatMapsAsTextures(const FString& HeightmapPngPath,
                                                     ALandscape* Landscape)
{
    const FString Dir    = FPaths::GetPath(HeightmapPngPath);
    FString       Prefix = FPaths::GetBaseFilename(HeightmapPngPath)
                               .Replace(TEXT("_heightmap"), TEXT(""));

    // Look for composite colormap first (preferred), then individual splat PNGs
    FString ColormapPath = FPaths::Combine(Dir,
        FString::Printf(TEXT("%s_colormap.png"), *Prefix));

    const TArray<FString> Layers = { TEXT("grass"), TEXT("rock"), TEXT("sand"), TEXT("snow") };
    TArray<FString> FilesToImport;

    if (FPaths::FileExists(ColormapPath))
        FilesToImport.Add(ColormapPath);

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

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ALandscape> WeakLandscape(Landscape);

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, WeakLandscape, FilesToImport, Prefix](float) -> bool
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            ALandscape* Land = WeakLandscape.Get();
            if (!Self) return false;

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

            // Auto-apply colormap as landscape material
            if (Land)
            {
                FString ColormapAsset = FString::Printf(
                    TEXT("/Game/LevelTool/SplatMaps/%s_colormap.%s_colormap"), *Prefix, *Prefix);
                UTexture2D* ColormapTex = LoadObject<UTexture2D>(nullptr, *ColormapAsset);

                if (ColormapTex)
                {
                    // Create a simple material that projects the colormap onto the landscape
                    static const TCHAR* MatPath = TEXT("/Game/LevelTool/M_LandscapeAuto.M_LandscapeAuto");
                    UMaterial* LandMat = LoadObject<UMaterial>(nullptr, MatPath);

                    if (!LandMat)
                    {
                        FString PkgPath = TEXT("/Game/LevelTool/M_LandscapeAuto");
                        UPackage* Pkg = CreatePackage(*PkgPath);
                        Pkg->FullyLoad();

                        LandMat = NewObject<UMaterial>(Pkg, TEXT("M_LandscapeAuto"), RF_Public | RF_Standalone);

                        // Texture coordinate: scale UVs to cover the entire landscape (0..1)
                        auto* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(LandMat);
                        TexCoord->CoordinateIndex = 0;

                        auto* TexSample = NewObject<UMaterialExpressionTextureSampleParameter2D>(LandMat);
                        TexSample->ParameterName = TEXT("Colormap");
                        TexSample->Texture = ColormapTex;
                        TexSample->SamplerType = SAMPLERTYPE_Color;
                        TexSample->Coordinates.Connect(0, TexCoord);

                        auto* RoughParam = NewObject<UMaterialExpressionScalarParameter>(LandMat);
                        RoughParam->ParameterName = TEXT("Roughness");
                        RoughParam->DefaultValue  = 0.85f;

                        auto& Exprs = LandMat->GetExpressionCollection().Expressions;
                        Exprs.Add(TexCoord);
                        Exprs.Add(TexSample);
                        Exprs.Add(RoughParam);

                        auto* Ed = LandMat->GetEditorOnlyData();
                        Ed->BaseColor.Connect(0, TexSample);
                        Ed->Roughness.Connect(0, RoughParam);

                        LandMat->PreEditChange(nullptr);
                        LandMat->PostEditChange();

                        FAssetRegistryModule::AssetCreated(LandMat);
                        Pkg->MarkPackageDirty();
                        FString FilePath = FPackageName::LongPackageNameToFilename(
                            PkgPath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Pkg, LandMat, *FilePath, SaveArgs);

                        Self->Log(TEXT("  ✔ Landscape material M_LandscapeAuto created"));
                    }

                    // Apply material to landscape
                    Land->LandscapeMaterial = LandMat;
                    Land->MarkPackageDirty();
                    Land->PostEditChange();
                    Self->Log(TEXT("  ✔ Landscape material applied (colormap-based)"));
                }
                else
                {
                    Self->Log(TEXT("  ⚠ Colormap texture not found after import — assign splat maps manually"));
                }
            }

            return false;
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

    float DiagCm     = (LandscapeBounds.Max - LandscapeBounds.Min).Size2D();
    float PillarH    = FMath::Max(DiagCm * 0.08f, 3000.0f);
    float PillarW    = FMath::Max(DiagCm * 0.003f, 80.0f);
    float BaseZ      = LandscapeBounds.Max.Z;
    float TextSize   = FMath::Max(PillarH * 0.15f, 500.0f);

    FVector Center2D = FVector(
        (LandscapeBounds.Min.X + LandscapeBounds.Max.X) * 0.5f,
        (LandscapeBounds.Min.Y + LandscapeBounds.Max.Y) * 0.5f,
        0.f);

    auto SpawnMarker = [&](FVector XY_Pos, FLinearColor Color, const FString& Label, const FString& DirText)
    {
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

        Actor->SetActorScale3D(FVector(PillarW / 100.f, PillarW / 100.f, PillarH / 100.f));
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(*Folder);

        // Text label floating above the pillar
        FVector TextLoc(XY_Pos.X, XY_Pos.Y, BaseZ + PillarH + TextSize);
        FActorSpawnParameters TextParams;
        TextParams.bNoFail = true;
        ATextRenderActor* TextActor = World->SpawnActor<ATextRenderActor>(
            ATextRenderActor::StaticClass(), TextLoc, FRotator::ZeroRotator, TextParams);
        if (TextActor)
        {
            UTextRenderComponent* TC = TextActor->GetTextRender();
            TC->SetText(FText::FromString(DirText));
            TC->SetTextRenderColor(Color.ToFColor(true));
            TC->SetWorldSize(TextSize);
            TC->SetHorizontalAlignment(EHTA_Center);
            TC->SetVerticalAlignment(EVRTA_TextCenter);
            TextActor->SetActorLabel(Label + TEXT("_Label"));
            TextActor->SetFolderPath(*Folder);
        }
    };

    float EdgeOffset = FMath::Min(
        (LandscapeBounds.Max.X - LandscapeBounds.Min.X) * 0.45f,
        (LandscapeBounds.Max.Y - LandscapeBounds.Min.Y) * 0.45f);

    SpawnMarker(Center2D, FLinearColor(1.f, 0.f, 0.f), TEXT("Compass_CENTER"), TEXT("CENTER"));
    SpawnMarker(Center2D + FVector(0, -EdgeOffset, 0), FLinearColor(0.f, 0.4f, 1.f), TEXT("Compass_N"), TEXT("N"));
    SpawnMarker(Center2D + FVector(0, +EdgeOffset, 0), FLinearColor(1.f, 0.9f, 0.f), TEXT("Compass_S"), TEXT("S"));
    SpawnMarker(Center2D + FVector(+EdgeOffset, 0, 0), FLinearColor(0.f, 0.9f, 0.f), TEXT("Compass_E"), TEXT("E"));
    SpawnMarker(Center2D + FVector(-EdgeOffset, 0, 0), FLinearColor(1.f, 0.5f, 0.f), TEXT("Compass_W"), TEXT("W"));

    Log(TEXT("  ✔ Compass markers spawned with labels (LevelTool/Compass)"));
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
        Entry.OsmId       = (int64)Obj->GetNumberField(TEXT("id"));
        Entry.TypeKey     = Obj->GetStringField(TEXT("type"));
        Entry.HeightM     = (float)Obj->GetNumberField(TEXT("height_m"));
        Entry.MinHeightM  = (float)Obj->GetNumberField(TEXT("min_height_m"));
        Entry.AreaM2      = (float)Obj->GetNumberField(TEXT("area_m2"));

        const TArray<TSharedPtr<FJsonValue>>& Centroid =
            Obj->GetArrayField(TEXT("centroid_ue5"));
        if (Centroid.Num() >= 2)
        {
            Entry.CentroidUE5.X = (float)Centroid[0]->AsNumber();
            Entry.CentroidUE5.Y = (float)Centroid[1]->AsNumber();
        }

        const TArray<TSharedPtr<FJsonValue>>* FootprintArr;
        if (Obj->TryGetArrayField(TEXT("footprint_ue5"), FootprintArr))
        {
            for (const TSharedPtr<FJsonValue>& PtVal : *FootprintArr)
            {
                const TArray<TSharedPtr<FJsonValue>>& Pt = PtVal->AsArray();
                if (Pt.Num() >= 2)
                {
                    Entry.FootprintUE5.Add(FVector2D(
                        (float)Pt[0]->AsNumber(), (float)Pt[1]->AsNumber()));
                }
            }
        }

        // OSM metadata
        Obj->TryGetStringField(TEXT("name"), Entry.Name);
        Obj->TryGetStringField(TEXT("roof_shape"), Entry.RoofShape);
        Obj->TryGetStringField(TEXT("building_colour"), Entry.BuildingColour);
        Obj->TryGetStringField(TEXT("building_material"), Entry.BuildingMaterial);
        Entry.Levels = (int32)Obj->GetNumberField(TEXT("levels"));

        OutBuildings.Add(Entry);
    }

    return true;
}

// Grid-based 2D spatial hash for O(1) overlap detection
namespace
{
    struct FSpatialHash2D
    {
        float CellSize;
        TMap<int64, TArray<FBox2D>> Cells;

        FSpatialHash2D(float InCellSize = 3000.f) : CellSize(InCellSize) {}

        int64 CellKey(int32 CX, int32 CY) const
        {
            return (static_cast<int64>(CX) << 32) | static_cast<int64>(static_cast<uint32>(CY));
        }

        void Insert(const FBox2D& Box)
        {
            int32 MinCX = FMath::FloorToInt32(Box.Min.X / CellSize);
            int32 MinCY = FMath::FloorToInt32(Box.Min.Y / CellSize);
            int32 MaxCX = FMath::FloorToInt32(Box.Max.X / CellSize);
            int32 MaxCY = FMath::FloorToInt32(Box.Max.Y / CellSize);
            for (int32 cx = MinCX; cx <= MaxCX; cx++)
                for (int32 cy = MinCY; cy <= MaxCY; cy++)
                    Cells.FindOrAdd(CellKey(cx, cy)).Add(Box);
        }

        bool Overlaps(const FBox2D& TestBox) const
        {
            int32 MinCX = FMath::FloorToInt32(TestBox.Min.X / CellSize);
            int32 MinCY = FMath::FloorToInt32(TestBox.Min.Y / CellSize);
            int32 MaxCX = FMath::FloorToInt32(TestBox.Max.X / CellSize);
            int32 MaxCY = FMath::FloorToInt32(TestBox.Max.Y / CellSize);
            for (int32 cx = MinCX; cx <= MaxCX; cx++)
            {
                for (int32 cy = MinCY; cy <= MaxCY; cy++)
                {
                    const TArray<FBox2D>* Arr = Cells.Find(CellKey(cx, cy));
                    if (!Arr) continue;
                    for (const FBox2D& Existing : *Arr)
                    {
                        if (TestBox.Min.X < Existing.Max.X && TestBox.Max.X > Existing.Min.X &&
                            TestBox.Min.Y < Existing.Max.Y && TestBox.Max.Y > Existing.Min.Y)
                            return true;
                    }
                }
            }
            return false;
        }
    };
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

    // Preload all meshes from the pool to avoid per-building LoadSynchronous
    TMap<FString, UStaticMesh*> MeshCache;
    if (Pool)
    {
        for (const FBuildingMeshEntry& Entry : Pool->Entries)
        {
            if (UStaticMesh* M = Entry.Mesh.LoadSynchronous())
                MeshCache.Add(Entry.TypeKey, M);
            for (const TSoftObjectPtr<UStaticMesh>& Var : Entry.MeshVariants)
                Var.LoadSynchronous();
        }
        Pool->FallbackMesh.LoadSynchronous();
        Log(FString::Printf(TEXT("  Preloaded %d mesh types from pool"), MeshCache.Num()));
    }

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

    TMap<FString, int32> TypeCounts;
    int32 Placed      = 0;
    int32 Skipped     = 0;
    int32 OutOfBounds = 0;
    int32 Overlapped  = 0;

    const FString FolderPath = TEXT("LevelTool/Buildings");

    FSpatialHash2D SpatialHash(3000.f);

    for (const FBuildingEntry& Building : Buildings)
    {
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

        FBox2D NewBox;
        if (Building.FootprintUE5.Num() >= 3)
        {
            NewBox = FBox2D(Building.FootprintUE5[0], Building.FootprintUE5[0]);
            for (const FVector2D& P : Building.FootprintUE5)
            {
                NewBox.Min.X = FMath::Min(NewBox.Min.X, P.X);
                NewBox.Min.Y = FMath::Min(NewBox.Min.Y, P.Y);
                NewBox.Max.X = FMath::Max(NewBox.Max.X, P.X);
                NewBox.Max.Y = FMath::Max(NewBox.Max.Y, P.Y);
            }
        }
        else
        {
            float HalfSide = FMath::Sqrt(FMath::Max(Building.AreaM2, 1.0f)) * 50.0f;
            NewBox.Min = Building.CentroidUE5 - FVector2D(HalfSide, HalfSide);
            NewBox.Max = Building.CentroidUE5 + FVector2D(HalfSide, HalfSide);
        }

        // Shrink test box slightly (90%) to allow touching but not overlapping
        FVector2D Center = (NewBox.Min + NewBox.Max) * 0.5f;
        FVector2D HalfExt = (NewBox.Max - NewBox.Min) * 0.45f;
        FBox2D TestBox(Center - HalfExt, Center + HalfExt);

        if (SpatialHash.Overlaps(TestBox))
        {
            Overlapped++;
            continue;
        }

        const float GroundZ = GetTerrainZAtWorldXY(Building.CentroidUE5.X, Building.CentroidUE5.Y) + ZOffsetCm;

        AStaticMeshActor* Actor = SpawnBuildingActor(Building, Pool, GroundZ);
        if (Actor)
        {
            Actor->SetFolderPath(*FolderPath);
            Placed++;
            TypeCounts.FindOrAdd(Building.TypeKey)++;
            SpatialHash.Insert(NewBox);
        }
        else
        {
            Skipped++;
        }
    }

    if (OutOfBounds > 0)
        Log(FString::Printf(TEXT("  ⚠ Skipped %d buildings outside Landscape bounds"), OutOfBounds));
    if (Overlapped > 0)
        Log(FString::Printf(TEXT("  ⚠ Skipped %d overlapping buildings"), Overlapped));

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

// ─────────────────────────────────────────────────────────────────────────────
//  B-B: Building type → color palette
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    struct FBuildingPalette
    {
        FLinearColor Tint;
        float Roughness;
        float Metallic;
    };

    FLinearColor ParseOsmColour(const FString& ColourStr)
    {
        if (ColourStr.IsEmpty()) return FLinearColor(-1,-1,-1,-1); // sentinel: not set
        FString C = ColourStr.ToLower().TrimStartAndEnd();

        // Named colours commonly used in OSM
        if (C == TEXT("white"))  return FLinearColor(0.85f, 0.85f, 0.85f, 1.f);
        if (C == TEXT("grey") || C == TEXT("gray")) return FLinearColor(0.55f, 0.55f, 0.55f, 1.f);
        if (C == TEXT("brown"))  return FLinearColor(0.45f, 0.30f, 0.18f, 1.f);
        if (C == TEXT("red"))    return FLinearColor(0.65f, 0.22f, 0.15f, 1.f);
        if (C == TEXT("yellow")) return FLinearColor(0.82f, 0.75f, 0.35f, 1.f);
        if (C == TEXT("beige"))  return FLinearColor(0.76f, 0.70f, 0.55f, 1.f);
        if (C == TEXT("blue"))   return FLinearColor(0.30f, 0.45f, 0.65f, 1.f);
        if (C == TEXT("green"))  return FLinearColor(0.30f, 0.55f, 0.30f, 1.f);
        if (C == TEXT("black"))  return FLinearColor(0.15f, 0.15f, 0.15f, 1.f);
        if (C == TEXT("orange")) return FLinearColor(0.78f, 0.50f, 0.20f, 1.f);
        if (C == TEXT("pink"))   return FLinearColor(0.80f, 0.55f, 0.58f, 1.f);

        // Try hex (#RRGGBB or #RGB)
        if (C.StartsWith(TEXT("#")) && (C.Len() == 7 || C.Len() == 4))
        {
            FColor Parsed = FColor::FromHex(C);
            return FLinearColor(Parsed);
        }
        return FLinearColor(-1,-1,-1,-1);
    }

    void ApplyMaterialOverrides(FBuildingPalette& Pal, const FString& MaterialStr)
    {
        if (MaterialStr.IsEmpty()) return;
        FString M = MaterialStr.ToLower();
        if (M == TEXT("glass"))         { Pal.Roughness = 0.10f; Pal.Metallic = 0.60f; }
        else if (M == TEXT("metal"))    { Pal.Roughness = 0.25f; Pal.Metallic = 0.80f; }
        else if (M == TEXT("brick"))    { Pal.Roughness = 0.90f; Pal.Metallic = 0.00f; }
        else if (M == TEXT("concrete")) { Pal.Roughness = 0.85f; Pal.Metallic = 0.02f; }
        else if (M == TEXT("wood"))     { Pal.Roughness = 0.92f; Pal.Metallic = 0.00f; }
        else if (M == TEXT("stone"))    { Pal.Roughness = 0.88f; Pal.Metallic = 0.01f; }
    }

    FBuildingPalette GetBuildingPalette(const FString& TypeKey, uint32 Hash,
        const FString& OsmColour = TEXT(""), const FString& OsmMaterial = TEXT(""))
    {
        float V = static_cast<float>(Hash & 0xFFFF) / 65535.f * 0.08f - 0.04f;

        // TypeKey comes as "BP_Building_Commercial" from Python — strip prefix and lowercase
        FString Key = TypeKey;
        Key.RemoveFromStart(TEXT("BP_Building_"));
        Key = Key.ToLower();

        FBuildingPalette Pal;

        if (Key == TEXT("commercial") || Key == TEXT("retail") ||
            Key == TEXT("office")     || Key == TEXT("hotel"))
            Pal = { FLinearColor(0.50f+V, 0.55f+V, 0.65f+V, 1.f), 0.30f, 0.40f };
        else if (Key == TEXT("residential") || Key == TEXT("apartment") ||
            Key == TEXT("house")       || Key == TEXT("detached") ||
            Key == TEXT("terrace")     || Key == TEXT("dormitory"))
            Pal = { FLinearColor(0.72f+V, 0.65f+V, 0.55f+V, 1.f), 0.85f, 0.0f };
        else if (Key == TEXT("industrial") || Key == TEXT("warehouse") ||
            Key == TEXT("factory")    || Key == TEXT("manufacture"))
            Pal = { FLinearColor(0.48f+V, 0.48f+V, 0.46f+V, 1.f), 0.90f, 0.05f };
        else if (Key == TEXT("church") || Key == TEXT("temple") ||
            Key == TEXT("shrine") || Key == TEXT("religious") ||
            Key == TEXT("cathedral"))
            Pal = { FLinearColor(0.76f+V, 0.70f+V, 0.60f+V, 1.f), 0.90f, 0.0f };
        else if (Key == TEXT("hospital") || Key == TEXT("school") ||
            Key == TEXT("university") || Key == TEXT("public") ||
            Key == TEXT("civic")      || Key == TEXT("government"))
            Pal = { FLinearColor(0.82f+V, 0.82f+V, 0.80f+V, 1.f), 0.60f, 0.05f };
        else if (Key == TEXT("generic") || Key == TEXT("garage") ||
            Key == TEXT("parking")  || Key == TEXT("carport"))
            Pal = { FLinearColor(0.55f+V, 0.54f+V, 0.52f+V, 1.f), 0.92f, 0.0f };
        else
            Pal = { FLinearColor(0.62f+V, 0.60f+V, 0.55f+V, 1.f), 0.80f, 0.0f };

        // Override with OSM building:colour if available
        FLinearColor OsmTint = ParseOsmColour(OsmColour);
        if (OsmTint.A > 0.f)
            Pal.Tint = OsmTint;

        // Override roughness/metallic from building:material
        ApplyMaterialOverrides(Pal, OsmMaterial);

        return Pal;
    }
}

AStaticMeshActor* ULevelToolSubsystem::SpawnBuildingActor(
    const FBuildingEntry& Building, ULevelToolBuildingPool* Pool, float GroundZ)
{
    UStaticMesh* Mesh = Pool ? Pool->ResolveMesh(Building.TypeKey) : nullptr;
    if (!Mesh)
    {
        Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    }
    if (!Mesh) return nullptr;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    FActorSpawnParameters Params;
    Params.Name         = FName(*FString::Printf(TEXT("Building_%lld"), Building.OsmId));
    Params.NameMode     = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    Params.bNoFail      = false;

    // Compute scale before spawning so we can offset Z correctly
    FVector Scale = ComputeBuildingScale(Building, Mesh);

    // B1: Derive yaw rotation from longest footprint edge
    float Yaw = 0.f;
    if (Building.FootprintUE5.Num() >= 2)
    {
        float MaxLenSq = 0.f;
        FVector2D BestDir(1.f, 0.f);
        for (int32 i = 0; i < Building.FootprintUE5.Num() - 1; i++)
        {
            FVector2D Edge = Building.FootprintUE5[i + 1] - Building.FootprintUE5[i];
            float LenSq = Edge.SizeSquared();
            if (LenSq > MaxLenSq)
            {
                MaxLenSq = LenSq;
                BestDir  = Edge;
            }
        }
        if (MaxLenSq > 0.01f)
        {
            Yaw = FMath::RadiansToDegrees(FMath::Atan2(BestDir.Y, BestDir.X));
        }
    }
    FRotator Rotation(0.f, Yaw, 0.f);

    // Place the actor so the mesh BOTTOM sits exactly on GroundZ.
    FBoxSphereBounds Bounds = Mesh->GetBounds();
    float BottomOffsetCm    = (Bounds.BoxExtent.Z - Bounds.Origin.Z) * Scale.Z;
    float MinHeightOffsetCm = Building.MinHeightM * 100.0f;
    FVector Location(Building.CentroidUE5.X, Building.CentroidUE5.Y,
                     GroundZ + BottomOffsetCm + MinHeightOffsetCm);

    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), Location, Rotation, Params);

    if (!Actor) return nullptr;

    UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
    Comp->SetStaticMesh(Mesh);
    Comp->SetCollisionProfileName(TEXT("BlockAll"));

    // Apply Nanite if requested by pool entry and supported by mesh
    if (Pool)
    {
        if (const FBuildingMeshEntry* Entry = Pool->FindEntry(Building.TypeKey))
        {
            if (Entry->bEnableNanite && Mesh->NaniteSettings.bEnabled == false)
            {
                Mesh->NaniteSettings.bEnabled = true;
                Mesh->PostEditChange();
                UE_LOG(LogLevelTool, Log, TEXT("  Nanite enabled on mesh: %s"), *Mesh->GetName());
            }
        }
    }

    Actor->SetActorRelativeScale3D(Scale);

    // Label
    Actor->SetActorLabel(FString::Printf(TEXT("Building_%lld"), Building.OsmId));

    // Tags for gameplay/PCG
    Actor->Tags.Add(*Building.TypeKey);
    Actor->Tags.Add(*FString::Printf(TEXT("h_%dm"), FMath::RoundToInt(Building.HeightM)));
    Actor->Tags.Add(*FString::Printf(TEXT("osm_%lld"), Building.OsmId));

    // ── Apply material: type-specific entry → procedural window → BasicShapeMaterial color ──
    bool bMaterialApplied = false;

    // Only use Pool materials if a SPECIFIC entry has a material set for this type
    if (Pool)
    {
        if (const FBuildingMeshEntry* Entry = Pool->FindEntry(Building.TypeKey))
        {
            UMaterialInterface* WallMat = Entry->WallMaterial.LoadSynchronous();
            UMaterialInterface* RoofMat = Entry->RoofMaterial.LoadSynchronous();
            if (WallMat) { Comp->SetMaterial(0, WallMat); bMaterialApplied = true; }
            if (RoofMat) Comp->SetMaterial(1, RoofMat);
        }
    }

    if (!bMaterialApplied)
    {
        uint32 OsmHash = static_cast<uint32>(Building.OsmId * 2654435761u);
        FBuildingPalette Pal = GetBuildingPalette(Building.TypeKey, OsmHash,
            Building.BuildingColour, Building.BuildingMaterial);

        // Try procedural window material first (creates once, reused for all buildings)
        if (!CachedBuildingMaterial) EnsureBuildingMaterial();

        if (CachedBuildingMaterial)
        {
            UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(CachedBuildingMaterial, Actor);
            DynMat->SetVectorParameterValue(TEXT("Tint"), Pal.Tint);
            DynMat->SetScalarParameterValue(TEXT("Roughness"), Pal.Roughness);
            DynMat->SetScalarParameterValue(TEXT("Metallic"), Pal.Metallic);

            // Tiling: more window rows for taller buildings
            float Floors = FMath::Max(1.f, Building.HeightM / 3.0f);
            DynMat->SetScalarParameterValue(TEXT("UTilingX"), 2.f);
            DynMat->SetScalarParameterValue(TEXT("UTilingY"), Floors);

            if (CachedWindowTexture)
                DynMat->SetTextureParameterValue(TEXT("WindowTex"), CachedWindowTexture);

            Comp->SetMaterial(0, DynMat);
        }
        else
        {
            UMaterialInterface* BasicMat = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
            if (BasicMat)
            {
                UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(BasicMat, Actor);
                DynMat->SetVectorParameterValue(TEXT("Color"), Pal.Tint);
                Comp->SetMaterial(0, DynMat);
            }
        }
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

    // Clamp extreme heights to prevent visual spikes
    constexpr float AbsMaxHeightM = 200.0f;
    float ClampedHeightM = FMath::Min(Building.HeightM, AbsMaxHeightM);

    float TargetHeightCm = ClampedHeightM * 100.0f;
    float HashFrac       = static_cast<float>((Building.OsmId * 2654435761u) & 0xFFFF) / 65535.0f;
    float ZVariation     = 0.9f + HashFrac * 0.2f;   // range [0.9, 1.1]
    float ZScale         = (TargetHeightCm / MeshHeightCm) * ZVariation;

    // XY: OBB from footprint polygon for accurate per-axis scaling
    float XScale, YScale;
    if (Building.FootprintUE5.Num() >= 3)
    {
        float MaxLenSq = 0.f;
        FVector2D MajorDir(1.f, 0.f);
        for (int32 i = 0; i < Building.FootprintUE5.Num() - 1; i++)
        {
            FVector2D Edge = Building.FootprintUE5[i + 1] - Building.FootprintUE5[i];
            float LenSq = Edge.SizeSquared();
            if (LenSq > MaxLenSq)
            {
                MaxLenSq = LenSq;
                MajorDir = Edge;
            }
        }
        MajorDir.Normalize();
        FVector2D MinorDir(-MajorDir.Y, MajorDir.X);

        float MinMaj = TNumericLimits<float>::Max(), MaxMaj = TNumericLimits<float>::Lowest();
        float MinMin = TNumericLimits<float>::Max(), MaxMin = TNumericLimits<float>::Lowest();
        for (const FVector2D& P : Building.FootprintUE5)
        {
            float ProjMaj = FVector2D::DotProduct(P, MajorDir);
            float ProjMin = FVector2D::DotProduct(P, MinorDir);
            MinMaj = FMath::Min(MinMaj, ProjMaj);
            MaxMaj = FMath::Max(MaxMaj, ProjMaj);
            MinMin = FMath::Min(MinMin, ProjMin);
            MaxMin = FMath::Max(MaxMin, ProjMin);
        }

        float ObbMajorCm = FMath::Max(MaxMaj - MinMaj, 1.f);
        float ObbMinorCm = FMath::Max(MaxMin - MinMin, 1.f);
        XScale = ObbMajorCm / MeshXCm;
        YScale = ObbMinorCm / MeshYCm;
    }
    else
    {
        float SideCm = FMath::Sqrt(FMath::Max(Building.AreaM2, 1.0f)) * 100.0f;
        XScale = SideCm / MeshXCm;
        YScale = SideCm / MeshYCm;
    }

    return FVector(FMath::Max(XScale, 0.1f), FMath::Max(YScale, 0.1f), FMath::Max(ZScale, 0.1f));
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

void ULevelToolSubsystem::EnsureBuildingMaterial()
{
    if (CachedBuildingMaterial) return;

    // Try loading previously saved material
    static const TCHAR* MatPath = TEXT("/Game/LevelTool/M_BuildingProcedural.M_BuildingProcedural");
    CachedBuildingMaterial = LoadObject<UMaterial>(nullptr, MatPath);
    if (CachedBuildingMaterial)
    {
        // Recreate runtime texture (not saved to disk) and override via MID
        CachedWindowTexture = nullptr;
        CreateWindowGridTexture();
        Log(TEXT("  ✔ Loaded saved building material from /Game/LevelTool/"));
        return;
    }

    // ── Create runtime window grid texture ──
    CreateWindowGridTexture();

    // ── Create material in a saveable package (not transient) ──
    FString PkgName = TEXT("/Game/LevelTool/M_BuildingProcedural");
    UPackage* Pkg = CreatePackage(*PkgName);
    Pkg->FullyLoad();

    CachedBuildingMaterial = NewObject<UMaterial>(
        Pkg, TEXT("M_BuildingProcedural"), RF_Public | RF_Standalone);
    CachedBuildingMaterial->TwoSided = false;

    auto* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(CachedBuildingMaterial);
    TexCoord->CoordinateIndex = 0;

    auto* ParamTX = NewObject<UMaterialExpressionScalarParameter>(CachedBuildingMaterial);
    ParamTX->ParameterName = TEXT("UTilingX");
    ParamTX->DefaultValue  = 1.0f;

    auto* ParamTY = NewObject<UMaterialExpressionScalarParameter>(CachedBuildingMaterial);
    ParamTY->ParameterName = TEXT("UTilingY");
    ParamTY->DefaultValue  = 1.0f;

    auto* AppendTiling = NewObject<UMaterialExpressionAppendVector>(CachedBuildingMaterial);
    AppendTiling->A.Connect(0, ParamTX);
    AppendTiling->B.Connect(0, ParamTY);

    auto* UVMul = NewObject<UMaterialExpressionMultiply>(CachedBuildingMaterial);
    UVMul->A.Connect(0, TexCoord);
    UVMul->B.Connect(0, AppendTiling);

    auto* TexSample = NewObject<UMaterialExpressionTextureSampleParameter2D>(CachedBuildingMaterial);
    TexSample->ParameterName = TEXT("WindowTex");
    TexSample->Texture       = CachedWindowTexture;
    TexSample->SamplerType   = SAMPLERTYPE_Color;
    TexSample->Coordinates.Connect(0, UVMul);

    auto* ParamTint = NewObject<UMaterialExpressionVectorParameter>(CachedBuildingMaterial);
    ParamTint->ParameterName = TEXT("Tint");
    ParamTint->DefaultValue  = FLinearColor(0.7f, 0.7f, 0.65f, 1.f);

    auto* ColorMul = NewObject<UMaterialExpressionMultiply>(CachedBuildingMaterial);
    ColorMul->A.Connect(0, TexSample);
    ColorMul->B.Connect(0, ParamTint);

    auto* ParamRough = NewObject<UMaterialExpressionScalarParameter>(CachedBuildingMaterial);
    ParamRough->ParameterName = TEXT("Roughness");
    ParamRough->DefaultValue  = 0.75f;

    auto* ParamMetal = NewObject<UMaterialExpressionScalarParameter>(CachedBuildingMaterial);
    ParamMetal->ParameterName = TEXT("Metallic");
    ParamMetal->DefaultValue  = 0.0f;

    auto& Exprs = CachedBuildingMaterial->GetExpressionCollection().Expressions;
    Exprs.Add(TexCoord);
    Exprs.Add(ParamTX);
    Exprs.Add(ParamTY);
    Exprs.Add(AppendTiling);
    Exprs.Add(UVMul);
    Exprs.Add(TexSample);
    Exprs.Add(ParamTint);
    Exprs.Add(ColorMul);
    Exprs.Add(ParamRough);
    Exprs.Add(ParamMetal);

    auto* Ed = CachedBuildingMaterial->GetEditorOnlyData();
    Ed->BaseColor.Connect(0, ColorMul);
    Ed->Roughness.Connect(0, ParamRough);
    Ed->Metallic.Connect(0, ParamMetal);

    CachedBuildingMaterial->PreEditChange(nullptr);
    CachedBuildingMaterial->PostEditChange();

    // Wait for shader compilation to complete synchronously
    if (GShaderCompilingManager)
    {
        GShaderCompilingManager->FinishAllCompilation();
    }

    // Save the material package to disk so shaders are cached for future runs
    FAssetRegistryModule::AssetCreated(CachedBuildingMaterial);
    Pkg->MarkPackageDirty();

    FString FilePath = FPackageName::LongPackageNameToFilename(
        PkgName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    UPackage::SavePackage(Pkg, CachedBuildingMaterial, *FilePath, SaveArgs);

    Log(TEXT("  ✔ Building material created & saved to /Game/LevelTool/"));
}

void ULevelToolSubsystem::CreateWindowGridTexture()
{
    if (CachedWindowTexture) return;

    constexpr int32 TS = 128;
    constexpr int32 Cell = TS / 4;
    constexpr int32 WL = 5, WR = 5;
    constexpr int32 WT = 5, WB = 7;
    constexpr int32 FLH = 2;

    CachedWindowTexture = UTexture2D::CreateTransient(TS, TS, PF_B8G8R8A8);
    CachedWindowTexture->Filter   = TF_Bilinear;
    CachedWindowTexture->AddressX = TA_Wrap;
    CachedWindowTexture->AddressY = TA_Wrap;
    CachedWindowTexture->SRGB     = true;

    FTexture2DMipMap& WMip = CachedWindowTexture->GetPlatformData()->Mips[0];
    FColor* WPx = static_cast<FColor*>(WMip.BulkData.Lock(LOCK_READ_WRITE));

    const FColor CWall   (185, 180, 170, 255);
    const FColor CGlass  ( 35,  45,  55, 255);
    const FColor CGlassHi( 55,  75,  95, 255);
    const FColor CFloor  (120, 115, 108, 255);

    for (int32 y = 0; y < TS; y++)
    {
        const int32 cy = y % Cell;
        for (int32 x = 0; x < TS; x++)
        {
            const int32 cx = x % Cell;
            FColor C = CWall;
            if (cy >= Cell - FLH)
                C = CFloor;
            else if (cx >= WL && cx < Cell - WR && cy >= WT && cy < Cell - WB)
                C = (cy < WT + 3) ? CGlassHi : CGlass;
            WPx[y * TS + x] = C;
        }
    }

    WMip.BulkData.Unlock();
    CachedWindowTexture->UpdateResource();
}

void ULevelToolSubsystem::Log(const FString& Msg)
{
    UE_LOG(LogLevelTool, Log, TEXT("%s"), *Msg);

    if (IsInGameThread())
    {
        LogLines.Add(Msg);
        OnLog.Broadcast(Msg);
    }
    else
    {
        TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Msg]()
        {
            if (ULevelToolSubsystem* Self = WeakThis.Get())
            {
                Self->LogLines.Add(Msg);
                Self->OnLog.Broadcast(Msg);
            }
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
        TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, Stage, Pct]()
        {
            if (ULevelToolSubsystem* Self = WeakThis.Get())
                Self->OnProgress.Broadcast(Stage, Pct);
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
        TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, bSuccess]()
        {
            if (ULevelToolSubsystem* Self = WeakThis.Get())
                Self->OnComplete.Broadcast(bSuccess);
        });
    }
}
