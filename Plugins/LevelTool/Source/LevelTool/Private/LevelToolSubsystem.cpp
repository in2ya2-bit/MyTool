#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"
#include "EditLayerManager.h"
#include "DesignerIntentSubsystem.h"

#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LevelEditor.h"
#include "EditorLevelLibrary.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IPythonScriptPlugin.h"


DEFINE_LOG_CATEGORY_STATIC(LogLevelTool, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    EditLayerManager = NewObject<UEditLayerManager>(this);
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

        {
            FString ScriptDir = FPaths::GetPath(Self->GetPythonScriptPath(TEXT("main.py")));
            FString MapName   = Preset.IsEmpty()
                ? FString::Printf(TEXT("custom_%.3f_%.3f"), Lat, Lon)
                : Preset;

            auto TryFallback = [&](FString& Path, const FString& SubDir, const FString& Suffix)
            {
                if (!Path.IsEmpty() && FPaths::FileExists(Path)) return;
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
            TryFallback(Result.WaterJsonPath,       TEXT("water"),      TEXT("_water.json"));
        }

        // ── Step 2: Import Landscape (game thread) ───────────────────────
        Async(EAsyncExecution::TaskGraphMainThread, [WeakThis, WeakPool, Result, bSpawnRoads, Gen, Preset, Lat, Lon, RadiusKm]() mutable
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
                Self->SetProgress(TEXT("Placing buildings..."), 0.70f);
                int32 Count = Self->PlaceBuildingsFromJson(Result.BuildingsJsonPath, WeakPool.Get(), 0.f);
                Result.BuildingCount = Count;
                Self->Log(FString::Printf(TEXT("✔ Placed %d building actors"), Count));
            }
            else if (!WeakPool.IsValid())
            {
                Self->Log(TEXT("ℹ No BuildingPool assigned — skipping actor placement"));
            }

            // ── Get landscape bounds (shared by roads + water) ────────
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

            // ── Step 4: Spawn Road Splines ───────────────────────────────
            if (bSpawnRoads && !Result.RoadsJsonPath.IsEmpty())
            {
                Self->SetProgress(TEXT("Spawning roads..."), 0.85f);
                Self->SpawnRoadActors(Result.RoadsJsonPath, LandscapeBounds);
            }

            // ── Step 5: Spawn Water Bodies ───────────────────────────────
            if (!Result.WaterJsonPath.IsEmpty())
            {
                Self->SetProgress(TEXT("Spawning water bodies..."), 0.92f);
                Self->SpawnWaterBodies(Result.WaterJsonPath, LandscapeBounds);
            }

            Self->LastResult = Result;
            Self->SetProgress(TEXT("Done"), 1.0f);

            // ── Step 6: Save map_meta.json + Initialize Edit Layer Manager ──
            {
                FString MapName = Preset.IsEmpty()
                    ? FString::Printf(TEXT("custom_%.3f_%.3f"), Lat, Lon)
                    : Preset;
                FString DateStr = FDateTime::UtcNow().ToString(TEXT("%Y%m%d"));
                FString MapId = FString::Printf(TEXT("%s_base_%s"), *MapName, *DateStr);
                Self->SaveMapMeta(MapId, Lat, Lon, RadiusKm, Result);

                if (Self->EditLayerManager)
                {
                    Self->EditLayerManager->Initialize(MapId);
                }

                if (auto* DIS = GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>())
                {
                    DIS->OnStage1Complete(MapId);
                }
            }

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
        if (Actor->GetFolderPath().ToString().StartsWith(TEXT("LevelTool")))
        {
            ToDelete.Add(Actor);
            continue;
        }
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
//  Python Execution  (with real-time progress streaming)
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

    static const FString ProgressMarker = TEXT("__LEVELTOOL_PROGRESS__:");

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
                if (Line.IsEmpty()) continue;

                if (Line.StartsWith(ProgressMarker))
                {
                    ParseAndBroadcastProgress(Line.Mid(ProgressMarker.Len()));
                }
                else
                {
                    Log(Line);
                }
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

    FString Remaining = FPlatformProcess::ReadPipe(ReadPipe);
    if (!Remaining.IsEmpty())
    {
        Stdout += Remaining;
        TArray<FString> RemLines;
        Remaining.ParseIntoArrayLines(RemLines);
        for (const FString& Line : RemLines)
        {
            if (Line.IsEmpty()) continue;

            if (Line.StartsWith(ProgressMarker))
            {
                ParseAndBroadcastProgress(Line.Mid(ProgressMarker.Len()));
            }
            else
            {
                Log(Line);
            }
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

void ULevelToolSubsystem::ParseAndBroadcastProgress(const FString& JsonStr)
{
    TSharedPtr<FJsonObject> Obj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
        return;

    FString Stage;
    double Percent = 0.0;
    Obj->TryGetStringField(TEXT("stage"), Stage);
    Obj->TryGetNumberField(TEXT("percent"), Percent);
    if (!Stage.IsEmpty())
        SetProgress(Stage, static_cast<float>(Percent));
}

FString ULevelToolSubsystem::BuildMainPyArgs(
    const FString& Command,
    const FString& Preset,
    float Lat, float Lon, float RadiusKm) const
{
    const ULevelToolSettings* S = ULevelToolSettings::Get();
    FString Args = Command;

    const float DiameterCm = RadiusKm * 2.0f * 100000.0f;
    float EffectiveXYScale = S->XYScaleCm;

    static const int32 ValidSizes[] = { 505, 1009, 2017, 4033 };
    int32 EffectiveGridSize = S->HeightmapSize;

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
        EffectiveGridSize = 4033;
        EffectiveXYScale = DiameterCm / (4033.f - 1.f);
        EffectiveXYScale = FMath::CeilToFloat(EffectiveXYScale);
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
    }

    return Args;
}

bool ULevelToolSubsystem::ParseJsonResultBlock(
    const FString& Output, FLevelToolJobResult& OutResult) const
{
    static const FString Marker = TEXT("__LEVELTOOL_RESULT__:");

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
    JsonObj->TryGetStringField(TEXT("water_json"), OutResult.WaterJsonPath);

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
    if (ParseJsonResultBlock(Stdout, OutResult))
        return;

    const ULevelToolSettings* S = ULevelToolSettings::Get();

    TArray<FString> Lines;
    Stdout.ParseIntoArrayLines(Lines);

    auto ExtractPath = [](const FString& Line, const FString& Prefix) -> FString
    {
        int32 Idx = Line.Find(Prefix);
        if (Idx == INDEX_NONE) return FString();
        FString After = Line.Mid(Idx + Prefix.Len()).TrimStart();
        int32 SpaceIdx = After.Find(TEXT("  "));
        if (SpaceIdx != INDEX_NONE)
            After = After.Left(SpaceIdx);
        return After.TrimStartAndEnd();
    };

    for (const FString& Line : Lines)
    {
        if (OutResult.HeightmapPngPath.IsEmpty()
            && Line.Contains(TEXT("_heightmap.png"))
            && Line.Contains(TEXT("File:")))
        {
            OutResult.HeightmapPngPath = ExtractPath(Line, TEXT("File:"));
        }
        else if (OutResult.HeightmapPngPath.IsEmpty()
                 && Line.Contains(TEXT("Heightmap PNG saved:")))
        {
            OutResult.HeightmapPngPath = ExtractPath(Line, TEXT("Heightmap PNG saved:"));
        }
        if (OutResult.BuildingsJsonPath.IsEmpty()
            && Line.Contains(TEXT("_buildings.json"))
            && Line.Contains(TEXT("Buildings JSON")))
        {
            FString Path = ExtractPath(Line, TEXT("Buildings JSON :"));
            if (Path.IsEmpty())
                Path = ExtractPath(Line, TEXT("Buildings JSON saved:"));
            if (!Path.IsEmpty())
                OutResult.BuildingsJsonPath = Path;
        }
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
        if (Line.Contains(TEXT("Elevation stats")))
        {
            FRegexMatcher MinM(FRegexPattern(TEXT("min=(-?[0-9]+\\.?[0-9]*)m")), Line);
            FRegexMatcher MaxM(FRegexPattern(TEXT("max=(-?[0-9]+\\.?[0-9]*)m")), Line);
            if (MinM.FindNext()) OutResult.ElevationMinM = FCString::Atof(*MinM.GetCaptureGroup(1));
            if (MaxM.FindNext()) OutResult.ElevationMaxM = FCString::Atof(*MaxM.GetCaptureGroup(1));
        }
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
        if (OutResult.HeightmapPngPath.IsEmpty() && Line.Contains(TEXT("Heightmap PNG")))
        {
            FString Path = ExtractPath(Line, TEXT("Heightmap PNG   :"));
            if (Path.IsEmpty()) Path = ExtractPath(Line, TEXT("Heightmap PNG :"));
            if (!Path.IsEmpty()) OutResult.HeightmapPngPath = Path;
        }
    }

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

// ─────────────────────────────────────────────────────────────────────────────
//  Map Meta: 1~3단계 공유 메타데이터 (stable_id 인프라 전제조건)
// ─────────────────────────────────────────────────────────────────────────────

FString ULevelToolSubsystem::GetEditLayerDir(const FString& MapId) const
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LevelTool"), TEXT("EditLayers"), MapId);
}

void ULevelToolSubsystem::SaveMapMeta(
    const FString& MapId, float Lat, float Lon, float RadiusKm,
    const FLevelToolJobResult& Result)
{
    FString Dir = GetEditLayerDir(MapId);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*Dir);

    FString MetaPath = FPaths::Combine(Dir, TEXT("map_meta.json"));

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema_version"), TEXT("1.0"));
    Root->SetStringField(TEXT("map_id"), MapId);

    FDateTime Now = FDateTime::UtcNow();
    Root->SetStringField(TEXT("created_at"), Now.ToIso8601());
    Root->SetStringField(TEXT("updated_at"), Now.ToIso8601());

    // origin
    TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
    Origin->SetNumberField(TEXT("lat"), Lat);
    Origin->SetNumberField(TEXT("lon"), Lon);
    Origin->SetNumberField(TEXT("radius_km"), RadiusKm);
    Origin->SetStringField(TEXT("source"), TEXT("osm"));
    Root->SetObjectField(TEXT("origin"), Origin);

    // terrain_profile
    TSharedRef<FJsonObject> Terrain = MakeShared<FJsonObject>();
    float ElevRange = Result.ElevationMaxM - Result.ElevationMinM;
    FString TerrainType;
    if (ElevRange < 100.f)
        TerrainType = TEXT("urban_dense");
    else if (ElevRange < 300.f)
        TerrainType = TEXT("mixed");
    else
        TerrainType = TEXT("mountain");
    Terrain->SetStringField(TEXT("type"), TerrainType);
    Terrain->SetNumberField(TEXT("min_elevation_m"), Result.ElevationMinM);
    Terrain->SetNumberField(TEXT("max_elevation_m"), Result.ElevationMaxM);
    Root->SetObjectField(TEXT("terrain_profile"), Terrain);

    // stage1_summary
    TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("building_count"), Result.BuildingCount);
    Summary->SetStringField(TEXT("generated_at"), Now.ToIso8601());
    Root->SetObjectField(TEXT("stage1_summary"), Summary);

    // slider_initial_values (2단계 진입 시 산출 — 여기서는 빈 오브젝트)
    Root->SetObjectField(TEXT("slider_initial_values"), MakeShared<FJsonObject>());

    // prediction_history (빈 배열)
    TArray<TSharedPtr<FJsonValue>> EmptyArr;
    Root->SetArrayField(TEXT("prediction_history"), EmptyArr);

    Root->SetStringField(TEXT("active_ruleset"), TEXT(""));

    FString OutputStr;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputStr);
    FJsonSerializer::Serialize(Root, Writer);

    if (FFileHelper::SaveStringToFile(OutputStr, *MetaPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Log(FString::Printf(TEXT("  ✔ map_meta.json saved: %s"), *MetaPath));
    }
    else
    {
        Log(FString::Printf(TEXT("  ⚠ Failed to save map_meta.json: %s"), *MetaPath));
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
