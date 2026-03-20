#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"
#include "LevelToolBuildingPool.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Engine/World.h"
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

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "ScopedTransaction.h"
#include "EngineUtils.h"
#include "Landscape.h"

DEFINE_LOG_CATEGORY_STATIC(LogLevelToolBuildings, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
//  Spatial hash for O(1) overlap detection
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
//  Building JSON loading
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

        Obj->TryGetStringField(TEXT("name"), Entry.Name);
        Obj->TryGetStringField(TEXT("roof_shape"), Entry.RoofShape);
        Obj->TryGetStringField(TEXT("building_colour"), Entry.BuildingColour);
        Obj->TryGetStringField(TEXT("building_material"), Entry.BuildingMaterial);
        Entry.Levels = (int32)Obj->GetNumberField(TEXT("levels"));

        OutBuildings.Add(Entry);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Building placement
// ─────────────────────────────────────────────────────────────────────────────

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
//  Building type → color palette
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
        if (ColourStr.IsEmpty()) return FLinearColor(-1,-1,-1,-1);
        FString C = ColourStr.ToLower().TrimStartAndEnd();

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

        FLinearColor OsmTint = ParseOsmColour(OsmColour);
        if (OsmTint.A > 0.f)
            Pal.Tint = OsmTint;

        ApplyMaterialOverrides(Pal, OsmMaterial);

        return Pal;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Spawn single building actor
// ─────────────────────────────────────────────────────────────────────────────

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

    FVector Scale = ComputeBuildingScale(Building, Mesh);

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

    if (Pool)
    {
        if (const FBuildingMeshEntry* Entry = Pool->FindEntry(Building.TypeKey))
        {
            if (Entry->bEnableNanite && !Mesh->IsNaniteEnabled())
            {
                FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
                NaniteSettings.bEnabled = true;
                Mesh->SetNaniteSettings(NaniteSettings);
                Mesh->PostEditChange();
                UE_LOG(LogLevelToolBuildings, Log, TEXT("  Nanite enabled on mesh: %s"), *Mesh->GetName());
            }
        }
    }

    Actor->SetActorRelativeScale3D(Scale);

    Actor->SetActorLabel(FString::Printf(TEXT("Building_%lld"), Building.OsmId));

    Actor->Tags.Add(*Building.TypeKey);
    Actor->Tags.Add(*FString::Printf(TEXT("h_%dm"), FMath::RoundToInt(Building.HeightM)));
    Actor->Tags.Add(*FString::Printf(TEXT("osm_%lld"), Building.OsmId));

    // stable_id 체계: 세션 독립적 Actor 식별 (2단계 Edit Layer 참조용)
    FString StableId = FString::Printf(TEXT("bldg_osm_%lld"), Building.OsmId);
    Actor->Tags.Add(FName(TEXT("LevelTool_StableID")));
    Actor->Tags.Add(FName(*StableId));
    Actor->Tags.Add(FName(TEXT("LevelTool_Generated")));

    bool bMaterialApplied = false;

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

        if (!CachedBuildingMaterial) EnsureBuildingMaterial();

        if (CachedBuildingMaterial)
        {
            UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(CachedBuildingMaterial, Actor);
            DynMat->SetVectorParameterValue(TEXT("Tint"), Pal.Tint);
            DynMat->SetScalarParameterValue(TEXT("Roughness"), Pal.Roughness);
            DynMat->SetScalarParameterValue(TEXT("Metallic"), Pal.Metallic);

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

// ─────────────────────────────────────────────────────────────────────────────
//  Building scale computation
// ─────────────────────────────────────────────────────────────────────────────

FVector ULevelToolSubsystem::ComputeBuildingScale(
    const FBuildingEntry& Building, UStaticMesh* Mesh) const
{
    float MeshHeightCm = 100.0f;
    float MeshXCm      = 100.0f;
    float MeshYCm      = 100.0f;

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

    constexpr float AbsMaxHeightM = 200.0f;
    float ClampedHeightM = FMath::Min(Building.HeightM, AbsMaxHeightM);

    float TargetHeightCm = ClampedHeightM * 100.0f;
    float HashFrac       = static_cast<float>((Building.OsmId * 2654435761u) & 0xFFFF) / 65535.0f;
    float ZVariation     = 0.9f + HashFrac * 0.2f;
    float ZScale         = (TargetHeightCm / MeshHeightCm) * ZVariation;

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
//  Procedural building material
// ─────────────────────────────────────────────────────────────────────────────

void ULevelToolSubsystem::EnsureBuildingMaterial()
{
    if (CachedBuildingMaterial) return;

    static const TCHAR* MatPath = TEXT("/Game/LevelTool/M_BuildingProcedural.M_BuildingProcedural");
    CachedBuildingMaterial = LoadObject<UMaterial>(nullptr, MatPath);
    if (CachedBuildingMaterial)
    {
        CachedWindowTexture = nullptr;
        CreateWindowGridTexture();
        Log(TEXT("  ✔ Loaded saved building material from /Game/LevelTool/"));
        return;
    }

    CreateWindowGridTexture();

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

    if (GShaderCompilingManager)
    {
        GShaderCompilingManager->FinishAllCompilation();
    }

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
