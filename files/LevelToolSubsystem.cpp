#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"

#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

// UE5 Landscape
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeEditorObject.h"
#include "LandscapeImportHelper.h"

// UE5 Level utilities
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "EditorLevelLibrary.h"
#include "LevelEditor.h"

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
    ULevelToolBuildingPool* Pool)
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

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool]()
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
            AsyncTask::ExecuteOnGameThread([WeakThis, Result]()
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

        Self->ParsePythonOutput(Stdout, Result);

        // ── Step 2: Import Landscape (game thread) ───────────────────────
        AsyncTask::ExecuteOnGameThread([WeakThis, WeakPool, Result]() mutable
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            if (!Self) return;

            Self->SetProgress(TEXT("Importing landscape..."), 0.50f);

            if (!Result.HeightmapPngPath.IsEmpty())
            {
                FString LandscapeName = FPaths::GetBaseFilename(Result.HeightmapPngPath)
                                          .Replace(TEXT("_heightmap"), TEXT(""));
                if (!Self->ImportHeightmapAsLandscape(Result.HeightmapPngPath, LandscapeName))
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

            Self->LastResult = Result;
            Self->SetProgress(TEXT("Done"), 1.0f);
            Self->FinishJob(true);
        });
    });
}

void ULevelToolSubsystem::RunLandscapeOnly(
    const FString& Preset, float Lat, float Lon, float RadiusKm)
{
    RunFullPipeline(Preset, Lat, Lon, RadiusKm, nullptr);
}

void ULevelToolSubsystem::RunBuildingsOnly(
    const FString& Preset, float Lat, float Lon, float RadiusKm,
    ULevelToolBuildingPool* Pool)
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

    AsyncTask = Async(EAsyncExecution::Thread, [WeakThis, Preset, Lat, Lon, RadiusKm, WeakPool]()
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
            AsyncTask::ExecuteOnGameThread([WeakThis, Result]() mutable
            {
                if (ULevelToolSubsystem* S = WeakThis.Get())
                {
                    S->LastResult = Result;
                    S->FinishJob(false);
                }
            });
            return;
        }

        Self->ParsePythonOutput(Stdout, Result);

        AsyncTask::ExecuteOnGameThread([WeakThis, WeakPool, Result]() mutable
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

    // Find python3 executable
    FString PythonExe = TEXT("python3");
#if PLATFORM_WINDOWS
    PythonExe = TEXT("python");
#endif

    FString CommandLine = FString::Printf(TEXT("\"%s\" %s"), *ScriptPath, *Args);

    Log(FString::Printf(TEXT("$ %s %s"), *PythonExe, *CommandLine));

    int32  ReturnCode = 0;
    FString Stdout, Stderr;

    bool bOk = FPlatformProcess::ExecProcess(
        *PythonExe,
        *CommandLine,
        &ReturnCode,
        &Stdout,
        &Stderr
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

    if (!Preset.IsEmpty())
    {
        Args += FString::Printf(TEXT(" --preset %s"), *Preset);
    }
    else
    {
        Args += FString::Printf(TEXT(" --lat %.6f --lon %.6f --radius %.2f"), Lat, Lon, RadiusKm);
    }

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

    for (const FString& Line : Lines)
    {
        if (Line.Contains(TEXT("_heightmap.png")))
        {
            // Extract path from "  heightmap_png         → Seoul_Jongno_heightmap.png"
            int32 ArrowIdx;
            if (Line.FindChar('→', ArrowIdx))
            {
                FString Filename = Line.Mid(ArrowIdx + 2).TrimStartAndEnd();
                OutResult.HeightmapPngPath = FPaths::Combine(
                    S->OutputDir.Path, TEXT("heightmaps"), Filename);
            }
        }
        else if (Line.Contains(TEXT("_buildings.json")))
        {
            int32 ArrowIdx;
            if (Line.FindChar('→', ArrowIdx))
            {
                FString Filename = Line.Mid(ArrowIdx + 2).TrimStartAndEnd();
                OutResult.BuildingsJsonPath = FPaths::Combine(
                    S->OutputDir.Path, TEXT("buildings"), Filename);
            }
        }
        else if (Line.Contains(TEXT("Elevation stats:")))
        {
            // "Elevation stats: min=150.9m  max=234.4m  ..."
            FRegexMatcher MinMatch(FRegexPattern(TEXT("min=([0-9.]+)m")), Line);
            FRegexMatcher MaxMatch(FRegexPattern(TEXT("max=([0-9.]+)m")), Line);
            if (MinMatch.FindNext())
                OutResult.ElevationMinM = FCString::Atof(*MinMatch.GetCaptureGroup(1));
            if (MaxMatch.FindNext())
                OutResult.ElevationMaxM = FCString::Atof(*MaxMatch.GetCaptureGroup(1));
        }
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
    const FString& HeightmapPngPath, const FString& LandscapeName)
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

    // Read heightmap file
    TArray<uint8> RawData;
    if (!FFileHelper::LoadFileToArray(RawData, *HeightmapPngPath))
    {
        Log(TEXT("✖ Failed to read heightmap file"));
        return false;
    }

    // Decode PNG to 16-bit
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> ImageWrapper =
        ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    if (!ImageWrapper->SetCompressed(RawData.GetData(), RawData.Num()))
    {
        Log(TEXT("✖ Failed to decode PNG heightmap"));
        return false;
    }

    TArray<uint8> UncompressedData;
    if (!ImageWrapper->GetRaw(ERGBFormat::Gray, 16, UncompressedData))
    {
        Log(TEXT("✖ Failed to get 16-bit grayscale data from PNG"));
        return false;
    }

    // Reinterpret as uint16
    const int32 NumVerts = S->HeightmapSize * S->HeightmapSize;
    TArray<uint16> HeightData;
    HeightData.SetNumUninitialized(NumVerts);
    FMemory::Memcpy(HeightData.GetData(), UncompressedData.GetData(),
                    NumVerts * sizeof(uint16));

    // Build landscape import layer info (empty = no paint layers initially)
    TArray<FLandscapeImportLayerInfo> ImportLayers;

    // Create landscape actor
    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "ImportLandscape", "LevelTool: Import Landscape"));

    FTransform LandscapeTransform(
        FQuat::Identity,
        FVector(0.f, 0.f, 0.f),
        FVector(S->XYScaleCm, S->XYScaleCm, S->GetLandscapeZScale())
    );

    ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(),
                                                          LandscapeTransform);
    if (!Landscape)
    {
        Log(TEXT("✖ Failed to spawn Landscape actor"));
        return false;
    }

    Landscape->SetActorLabel(LandscapeName);

    // Import heightmap data
    Landscape->Import(
        FGuid::NewGuid(),
        0, 0,                                        // min X/Y
        ComponentCountX * QuadsPerComp,              // max X
        ComponentCountY * QuadsPerComp,              // max Y
        SectionsPerComp,
        SectionSize,
        HeightData,
        nullptr,                                     // heightmap file path (not needed)
        ImportLayers,
        ELandscapeImportAlphamapType::Additive
    );

    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (LandscapeInfo)
    {
        LandscapeInfo->UpdateLayerInfoMap(Landscape);
    }

    Log(FString::Printf(TEXT("✔ Landscape created: %s  scale=%.0f×%.0f×%.3f  components=%d×%d"),
        *LandscapeName,
        S->XYScaleCm, S->XYScaleCm, S->GetLandscapeZScale(),
        ComponentCountX, ComponentCountY));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Building Placement
// ─────────────────────────────────────────────────────────────────────────────

bool ULevelToolSubsystem::LoadBuildingsJson(
    const FString& JsonPath, TArray<FBuildingEntry>& OutBuildings) const
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

    FScopedTransaction Transaction(
        NSLOCTEXT("LevelTool", "PlaceBuildings", "LevelTool: Place Buildings"));

    // Group log: counts per type
    TMap<FString, int32> TypeCounts;
    int32 Placed  = 0;
    int32 Skipped = 0;

    // Create a folder in the World Outliner
    const FString FolderPath = TEXT("LevelTool/Buildings");

    for (const FBuildingEntry& Building : Buildings)
    {
        AStaticMeshActor* Actor = SpawnBuildingActor(Building, Pool, ZOffsetCm);
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
    const FBuildingEntry& Building, ULevelToolBuildingPool* Pool, float ZOffsetCm)
{
    UStaticMesh* Mesh = Pool ? Pool->ResolveMesh(Building.TypeKey) : nullptr;
    if (!Mesh) return nullptr;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    FActorSpawnParameters Params;
    Params.Name         = FName(*FString::Printf(TEXT("Building_%lld"), Building.OsmId));
    Params.NameMode     = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    Params.bNoFail      = false;

    FVector Location(Building.CentroidUE5.X, Building.CentroidUE5.Y, ZOffsetCm);

    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, Params);

    if (!Actor) return nullptr;

    UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
    Comp->SetStaticMesh(Mesh);
    Comp->SetCollisionProfileName(TEXT("BlockAll"));

    // Scale Z to match real building height
    FVector Scale = ComputeBuildingScale(Building, Mesh);
    Actor->SetActorRelativeScale3D(Scale);

    // Label
    Actor->SetActorLabel(FString::Printf(TEXT("Building_%lld"), Building.OsmId));

    // Tags for gameplay/PCG
    Actor->Tags.Add(*Building.TypeKey);
    Actor->Tags.Add(*FString::Printf(TEXT("h_%dm"), FMath::RoundToInt(Building.HeightM)));
    Actor->Tags.Add(*FString::Printf(TEXT("osm_%lld"), Building.OsmId));

    // Apply material overrides from pool
    if (Pool)
    {
        if (const FBuildingMeshEntry* Entry = Pool->FindEntry(Building.TypeKey))
        {
            if (UMaterialInterface* WallMat = Entry->WallMaterial.LoadSynchronous())
                Comp->SetMaterial(0, WallMat);
            if (UMaterialInterface* RoofMat = Entry->RoofMaterial.LoadSynchronous())
                Comp->SetMaterial(1, RoofMat);
        }
    }

    return Actor;
}

FVector ULevelToolSubsystem::ComputeBuildingScale(
    const FBuildingEntry& Building, UStaticMesh* Mesh) const
{
    // Base assumption: mesh is 1m (100cm) tall at scale 1.0
    // We scale to match the OSM height
    float MeshHeightCm = 100.0f;

    // Try to read actual mesh bounds for accurate scaling
    if (Mesh)
    {
        FBoxSphereBounds Bounds = Mesh->GetBounds();
        float ActualHeightCm   = Bounds.BoxExtent.Z * 2.0f;
        if (ActualHeightCm > 1.0f)
            MeshHeightCm = ActualHeightCm;
    }

    float TargetHeightCm = Building.HeightM * 100.0f;
    float ZScale         = TargetHeightCm / MeshHeightCm;

    // Apply random variation
    ZScale *= FMath::RandRange(0.9f, 1.1f);

    return FVector(1.0f, 1.0f, FMath::Max(ZScale, 0.1f));
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
    LogLines.Add(Msg);
    UE_LOG(LogLevelTool, Log, TEXT("%s"), *Msg);
    OnLog.Broadcast(Msg);
}

void ULevelToolSubsystem::SetProgress(const FString& Stage, float Pct)
{
    Log(FString::Printf(TEXT("[%.0f%%] %s"), Pct * 100.f, *Stage));
    OnProgress.Broadcast(Stage, Pct);
}

void ULevelToolSubsystem::FinishJob(bool bSuccess)
{
    bJobRunning = false;
    OnComplete.Broadcast(bSuccess);
}
