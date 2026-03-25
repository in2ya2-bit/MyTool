#include "BuildingDataAsset.h"
#include "Engine/StaticMesh.h"
#include "UObject/SoftObjectPath.h"
#include "PhysicsEngine/BodySetup.h"

const FMeshSlot* UBuildingDataAsset::ResolveMeshSlot(
    ETileType TileType, EWallVariant Variant) const
{
    switch (TileType)
    {
    case ETileType::Wall:
        if (const FMeshSlot* Slot = WallMeshes.Find(Variant))
        {
            if (Slot->IsValid()) return Slot;
        }
        break;

    case ETileType::Wall_Door:
        if (DoorMesh.IsValid()) return &DoorMesh;
        break;

    case ETileType::Wall_Window:
        if (WindowMesh.IsValid()) return &WindowMesh;
        break;

    case ETileType::Stairs_Up:
    case ETileType::Stairs:
        if (StairsUpMesh.IsValid()) return &StairsUpMesh;
        break;

    case ETileType::Stairs_Down:
        if (StairsDownMesh.IsValid()) return &StairsDownMesh;
        break;

    case ETileType::Floor:
    case ETileType::Room_A:
    case ETileType::Room_B:
    case ETileType::Room_C:
    case ETileType::Corridor:
        if (const FMeshSlot* Slot = FloorMeshes.Find(TileType))
        {
            if (Slot->IsValid()) return Slot;
        }
        break;

    default:
        break;
    }

    if (FallbackMesh.IsValid())
        return &FallbackMesh;

    return nullptr;
}

TArray<FString> UBuildingDataAsset::Validate() const
{
    TArray<FString> Warnings;

    static const EWallVariant RequiredVariants[] = {
        EWallVariant::Isolated, EWallVariant::Straight, EWallVariant::Corner,
        EWallVariant::T_Junction, EWallVariant::Cross, EWallVariant::End
    };

    for (EWallVariant V : RequiredVariants)
    {
        if (!WallMeshes.Contains(V) || !WallMeshes[V].IsValid())
        {
            Warnings.Add(FString::Printf(
                TEXT("Wall variant %d missing mesh"), static_cast<int32>(V)));
        }
    }

    if (!DoorMesh.IsValid())
        Warnings.Add(TEXT("Door mesh not assigned"));
    if (!WindowMesh.IsValid())
        Warnings.Add(TEXT("Window mesh not assigned"));
    if (!StairsUpMesh.IsValid())
        Warnings.Add(TEXT("Stairs-Up mesh not assigned"));
    if (!StairsDownMesh.IsValid())
        Warnings.Add(TEXT("Stairs-Down mesh not assigned"));
    if (!CeilingMesh.IsValid())
        Warnings.Add(TEXT("Ceiling mesh not assigned — floors will have no ceiling"));
    if (!FallbackMesh.IsValid())
        Warnings.Add(TEXT("Fallback mesh not assigned — unresolvable tiles will be invisible"));

    return Warnings;
}

int32 UBuildingDataAsset::AutoPopulateFromDirectory(const FString& ContentPath)
{
    int32 AssignedCount = 0;
    constexpr float ExpectedTileHalf = 200.f;

    auto TryLoadMesh = [&](const FString& MeshName) -> UStaticMesh*
    {
        FString FullPath = ContentPath / MeshName + TEXT(".") + MeshName;
        return Cast<UStaticMesh>(
            FSoftObjectPath(FullPath).TryLoad());
    };

    auto DetectScaleCorrection = [&](UStaticMesh* Mesh, const FString& Name) -> FVector
    {
        FBox BBox = Mesh->GetBoundingBox();
        FVector Size = BBox.GetSize();
        float MaxDim = Size.GetMax();

        UE_LOG(LogTemp, Log, TEXT("  Mesh [%s] bounds: %s  size: %s"),
            *Name, *BBox.ToString(), *Size.ToString());

        if (MaxDim < 1.f)
        {
            return FVector::OneVector;
        }

        // Blender global_scale=100 + UE5 Interchange may cause 100x over-scale
        // Expected max dim for a tile-sized mesh: ~400cm (full tile) or ~300cm (wall height)
        // If actual max dim > 10x expected, apply correction
        constexpr float ExpectedMaxDim = 400.f;
        float Ratio = MaxDim / ExpectedMaxDim;

        if (Ratio > 5.f)
        {
            float Correction = ExpectedMaxDim / MaxDim;
            UE_LOG(LogTemp, Warning,
                TEXT("  Mesh [%s] appears %.0fx too large (%.0f cm). Applying ScaleCorrection=%.4f"),
                *Name, Ratio, MaxDim, Correction);
            return FVector(Correction);
        }

        return FVector::OneVector;
    };

    auto SetupCollision = [](UStaticMesh* Mesh)
    {
        if (!Mesh) return;

        UBodySetup* BS = Mesh->GetBodySetup();
        if (!BS)
        {
            Mesh->CreateBodySetup();
            BS = Mesh->GetBodySetup();
        }
        if (BS)
        {
            BS->CollisionTraceFlag = ECollisionTraceFlag::CTF_UseComplexAsSimple;
            Mesh->MarkPackageDirty();
        }
    };

    auto MakeSlot = [&](const FString& MeshName) -> FMeshSlot
    {
        FMeshSlot Slot;
        UStaticMesh* Loaded = TryLoadMesh(MeshName);
        if (Loaded)
        {
            Slot.Mesh = Loaded;
            Slot.ScaleCorrection = DetectScaleCorrection(Loaded, MeshName);
            Slot.PivotOffset = FVector(ExpectedTileHalf, ExpectedTileHalf, 0.f);
            SetupCollision(Loaded);
            ++AssignedCount;
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("  Mesh NOT FOUND: %s"), *MeshName);
        }
        return Slot;
    };

    struct FWallEntry
    {
        EWallVariant Variant;
        FString MeshName;
    };

    const FWallEntry WallEntries[] = {
        { EWallVariant::Isolated,   TEXT("SM_Wall_Isolated") },
        { EWallVariant::End,        TEXT("SM_Wall_End") },
        { EWallVariant::Straight,   TEXT("SM_Wall_Straight") },
        { EWallVariant::Corner,     TEXT("SM_Wall_Corner") },
        { EWallVariant::T_Junction, TEXT("SM_Wall_T_Junction") },
        { EWallVariant::Cross,      TEXT("SM_Wall_Cross") },
    };

    for (const FWallEntry& E : WallEntries)
    {
        FMeshSlot Slot = MakeSlot(E.MeshName);
        if (Slot.IsValid())
        {
            WallMeshes.Add(E.Variant, Slot);
        }
    }

    DoorMesh = MakeSlot(TEXT("SM_Wall_Door"));
    WindowMesh = MakeSlot(TEXT("SM_Wall_Window"));

    struct FFloorEntry
    {
        ETileType Type;
        FString MeshName;
    };

    const FFloorEntry FloorEntries[] = {
        { ETileType::Floor,    TEXT("SM_Floor_Generic") },
        { ETileType::Room_A,   TEXT("SM_Floor_Room_A") },
        { ETileType::Room_B,   TEXT("SM_Floor_Room_B") },
        { ETileType::Room_C,   TEXT("SM_Floor_Room_C") },
        { ETileType::Corridor, TEXT("SM_Floor_Corridor") },
    };

    for (const FFloorEntry& E : FloorEntries)
    {
        FMeshSlot Slot = MakeSlot(E.MeshName);
        if (Slot.IsValid())
        {
            FloorMeshes.Add(E.Type, Slot);
        }
    }

    StairsUpMesh = MakeSlot(TEXT("SM_Stairs_Up"));
    StairsDownMesh = MakeSlot(TEXT("SM_Stairs_Down"));

    CeilingMesh = MakeSlot(TEXT("SM_Floor_Generic"));

    FallbackMesh = MakeSlot(TEXT("SM_Wall_Straight"));

    if (MeshSourceDirectory.Path.IsEmpty())
    {
        MeshSourceDirectory.Path = ContentPath;
    }

    UE_LOG(LogTemp, Log, TEXT("BuildingDataAsset: AutoPopulate assigned %d mesh slots from [%s]"),
        AssignedCount, *ContentPath);

    return AssignedCount;
}
