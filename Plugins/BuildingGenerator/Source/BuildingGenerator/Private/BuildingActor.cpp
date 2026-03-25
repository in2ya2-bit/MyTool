#include "BuildingActor.h"
#include "BuildingDataAsset.h"
#include "BuildingAutoTile.h"
#include "BuildingModuleActor.h"
#include "Engine/StaticMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildingGen, Log, All);

static constexpr float BASE_TILE_SIZE = 400.f;
static constexpr float BASE_FLOOR_HEIGHT = 300.f;

ABuildingActor::ABuildingActor()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

    if (BuildingData.Floors.IsEmpty())
    {
        BuildingData.AddFloor(14, 10, TEXT("1F"));
    }
}

void ABuildingActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

    if (BuildingData.Floors.IsEmpty())
    {
        BuildingData.AddFloor(14, 10, TEXT("1F"));
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Key helpers
// ─────────────────────────────────────────────────────────────────────

FString ABuildingActor::MakeInstanceKey(int32 FloorIndex, int32 X, int32 Y) const
{
    return FString::Printf(TEXT("%d_%d_%d"), FloorIndex, X, Y);
}

// ─────────────────────────────────────────────────────────────────────
//  HISM bucket management
// ─────────────────────────────────────────────────────────────────────

UHierarchicalInstancedStaticMeshComponent* ABuildingActor::GetOrCreateBucket(
    const FString& MeshIdentity, UStaticMesh* Mesh)
{
    if (auto* Existing = HismBuckets.Find(MeshIdentity))
    {
        return *Existing;
    }

    FName CompName(*FString::Printf(TEXT("HISM_%s"), *FPaths::GetBaseFilename(MeshIdentity)));
    auto* Comp = NewObject<UHierarchicalInstancedStaticMeshComponent>(
        this, UHierarchicalInstancedStaticMeshComponent::StaticClass(), CompName);
    Comp->SetStaticMesh(Mesh);
    Comp->SetMobility(EComponentMobility::Movable);
    Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Comp->SetCollisionProfileName(TEXT("BlockAll"));
    Comp->SetGenerateOverlapEvents(false);
    Comp->bAutoRebuildTreeOnInstanceChanges = false;
    Comp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    Comp->RegisterComponent();

    HismBuckets.Add(MeshIdentity, Comp);
    return Comp;
}

void ABuildingActor::ClearAllBuckets()
{
    for (auto& Pair : HismBuckets)
    {
        if (Pair.Value)
        {
            Pair.Value->ClearInstances();
            Pair.Value->DestroyComponent();
        }
    }
    HismBuckets.Empty();
    InstanceMap.Empty();
}

// ─────────────────────────────────────────────────────────────────────
//  Mesh scale helpers
// ─────────────────────────────────────────────────────────────────────

void ABuildingActor::ComputeCachedMeshScale()
{
    CachedMeshScale = 1.f;
    if (!MeshSet)
        return;

    const FMeshSlot* RefSlot = MeshSet->FloorMeshes.Find(ETileType::Floor);
    if (!RefSlot || !RefSlot->IsValid())
        return;

    UStaticMesh* RefMesh = RefSlot->LoadMesh();
    if (!RefMesh)
        return;

    float MaxDim = RefMesh->GetBoundingBox().GetSize().GetMax();
    if (MaxDim < 1.f)
        return;

    CachedMeshScale = BASE_TILE_SIZE / MaxDim;

    UE_LOG(LogBuildingGen, Log,
        TEXT("Reference mesh MaxDim=%.1f cm → CachedMeshScale=%.4f"),
        MaxDim, CachedMeshScale);
}

FVector ABuildingActor::ComputeTilePivot() const
{
    return FVector(
        BuildingData.TileSize * 0.5f,
        BuildingData.TileSize * 0.5f,
        0.f);
}

// ─────────────────────────────────────────────────────────────────────
//  Instance add / hide / restore
// ─────────────────────────────────────────────────────────────────────

void ABuildingActor::AddTileInstance(
    int32 FloorIndex, int32 X, int32 Y, const FTileData& Tile)
{
    if (Tile.IsEmpty() || !MeshSet)
        return;

    const FMeshSlot* Slot = MeshSet->ResolveMeshSlot(Tile.TileType, Tile.WallVariant);
    if (!Slot)
        return;

    UStaticMesh* Mesh = Slot->LoadMesh();
    if (!Mesh)
        return;

    const float S   = BuildingData.BuildingScale;
    const float MS  = CachedMeshScale;
    const float SXY = BuildingData.TileSize / BASE_TILE_SIZE;
    const float SZ  = BuildingData.FloorHeight / BASE_FLOOR_HEIGHT;

    FString MeshIdentity = Mesh->GetPathName();
    UHierarchicalInstancedStaticMeshComponent* Comp = GetOrCreateBucket(MeshIdentity, Mesh);

    FVector Location = (BuildingData.TileToWorld(FloorIndex, X, Y) + ComputeTilePivot()) * S;
    FRotator Rotation(0.f, Tile.AutoRotationYaw, 0.f);
    FVector Scale(MS * SXY * S, MS * SXY * S, MS * SZ * S);
    FTransform InstanceTransform(Rotation, Location, Scale);

    int32 InstanceIndex = Comp->AddInstance(InstanceTransform, /*bWorldSpace=*/false);

    FString Key = MakeInstanceKey(FloorIndex, X, Y);
    FBuildingInstance& Inst = InstanceMap.FindOrAdd(Key);
    Inst.FloorIndex = FloorIndex;
    Inst.TileX = X;
    Inst.TileY = Y;
    Inst.HismInstanceIndex = InstanceIndex;
    Inst.BucketKey = MeshIdentity;
    Inst.bHidden = false;
}

void ABuildingActor::AddBaseFloorInstance(int32 FloorIndex, int32 X, int32 Y)
{
    if (!MeshSet)
        return;

    const FMeshSlot* FloorSlot = MeshSet->FloorMeshes.Find(ETileType::Floor);
    if (!FloorSlot || !FloorSlot->IsValid())
        return;

    UStaticMesh* Mesh = FloorSlot->LoadMesh();
    if (!Mesh)
        return;

    const float S   = BuildingData.BuildingScale;
    const float MS  = CachedMeshScale;
    const float SXY = BuildingData.TileSize / BASE_TILE_SIZE;
    const float SZ  = BuildingData.FloorHeight / BASE_FLOOR_HEIGHT;

    FString MeshIdentity = Mesh->GetPathName();
    UHierarchicalInstancedStaticMeshComponent* Comp = GetOrCreateBucket(MeshIdentity, Mesh);

    FVector Location = (BuildingData.TileToWorld(FloorIndex, X, Y) + ComputeTilePivot()) * S;
    FVector Scale(MS * SXY * S, MS * SXY * S, MS * SZ * S);
    FTransform InstanceTransform(FRotator::ZeroRotator, Location, Scale);
    Comp->AddInstance(InstanceTransform, /*bWorldSpace=*/false);
}

void ABuildingActor::AddCeilingInstance(int32 FloorIndex, int32 X, int32 Y)
{
    if (!MeshSet || !MeshSet->CeilingMesh.IsValid())
        return;

    const FMeshSlot& Slot = MeshSet->CeilingMesh;
    UStaticMesh* Mesh = Slot.LoadMesh();
    if (!Mesh)
        return;

    const float S   = BuildingData.BuildingScale;
    const float MS  = CachedMeshScale;
    const float SXY = BuildingData.TileSize / BASE_TILE_SIZE;
    const float SZ  = BuildingData.FloorHeight / BASE_FLOOR_HEIGHT;

    FString MeshIdentity = Mesh->GetPathName();
    UHierarchicalInstancedStaticMeshComponent* Comp = GetOrCreateBucket(MeshIdentity, Mesh);

    FVector Location = (BuildingData.TileToWorld(FloorIndex, X, Y) + ComputeTilePivot()) * S;
    Location.Z += (BuildingData.FloorHeight - 2.f) * S;
    FVector Scale(MS * SXY * S, MS * SXY * S, MS * SZ * S);
    FTransform InstanceTransform(FRotator::ZeroRotator, Location, Scale);

    Comp->AddInstance(InstanceTransform, /*bWorldSpace=*/false);
}

void ABuildingActor::HideInstance(int32 FloorIndex, int32 X, int32 Y)
{
    FString Key = MakeInstanceKey(FloorIndex, X, Y);
    FBuildingInstance* Inst = InstanceMap.Find(Key);
    if (!Inst || Inst->bHidden)
        return;

    auto* CompPtr = HismBuckets.Find(Inst->BucketKey);
    if (!CompPtr || !*CompPtr)
        return;

    FTransform ZeroScale;
    ZeroScale.SetScale3D(FVector::ZeroVector);
    (*CompPtr)->UpdateInstanceTransform(
        Inst->HismInstanceIndex, ZeroScale,
        /*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/true);

    Inst->bHidden = true;
}

void ABuildingActor::RestoreInstance(int32 FloorIndex, int32 X, int32 Y)
{
    FString Key = MakeInstanceKey(FloorIndex, X, Y);
    FBuildingInstance* Inst = InstanceMap.Find(Key);
    if (!Inst || !Inst->bHidden)
        return;

    if (!MeshSet || !BuildingData.Floors.IsValidIndex(FloorIndex))
        return;

    const FFloorData& Floor = BuildingData.Floors[FloorIndex];
    if (!Floor.IsValidCoord(X, Y))
        return;

    const FTileData& Tile = Floor.GetTile(X, Y);
    const FMeshSlot* Slot = MeshSet->ResolveMeshSlot(Tile.TileType, Tile.WallVariant);
    if (!Slot)
        return;

    auto* CompPtr = HismBuckets.Find(Inst->BucketKey);
    if (!CompPtr || !*CompPtr)
        return;

    const float S   = BuildingData.BuildingScale;
    const float MS  = CachedMeshScale;
    const float SXY = BuildingData.TileSize / BASE_TILE_SIZE;
    const float SZ  = BuildingData.FloorHeight / BASE_FLOOR_HEIGHT;

    FVector Location = (BuildingData.TileToWorld(FloorIndex, X, Y) + ComputeTilePivot()) * S;
    FRotator Rotation(0.f, Tile.AutoRotationYaw, 0.f);
    FVector Scale(MS * SXY * S, MS * SXY * S, MS * SZ * S);
    FTransform OriginalTransform(Rotation, Location, Scale);

    (*CompPtr)->UpdateInstanceTransform(
        Inst->HismInstanceIndex, OriginalTransform,
        /*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/true);

    Inst->bHidden = false;
}

void ABuildingActor::RepairHiddenInstances()
{
    for (auto& Pair : InstanceMap)
    {
        FBuildingInstance& Inst = Pair.Value;
        if (Inst.bHidden)
        {
            RestoreInstance(Inst.FloorIndex, Inst.TileX, Inst.TileY);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Full rebuild
// ─────────────────────────────────────────────────────────────────────

void ABuildingActor::RebuildHISM()
{
    DestroyAllEditProxies();
    ClearAllBuckets();

    if (!MeshSet)
    {
        UE_LOG(LogBuildingGen, Warning, TEXT("RebuildHISM: No MeshSet assigned"));
        return;
    }

    for (FFloorData& Floor : BuildingData.Floors)
    {
        Floor.EnsureTilesSize();
    }

    BuildingAutoTile::ProcessBuilding(BuildingData);
    ComputeCachedMeshScale();

    for (int32 F = 0; F < BuildingData.Floors.Num(); ++F)
    {
        const FFloorData& Floor = BuildingData.Floors[F];
        for (int32 Y = 0; Y < Floor.Height; ++Y)
        {
            for (int32 X = 0; X < Floor.Width; ++X)
            {
                const FTileData& Tile = Floor.GetTile(X, Y);
                AddTileInstance(F, X, Y, Tile);

                if (Tile.IsWallFamily() || Tile.IsStairs())
                {
                    AddBaseFloorInstance(F, X, Y);
                }

                if (!Tile.IsEmpty() && !Tile.IsStairs())
                {
                    AddCeilingInstance(F, X, Y);
                }
            }
        }
    }

    for (auto& Pair : HismBuckets)
    {
        if (Pair.Value)
        {
            Pair.Value->BuildTreeIfOutdated(true, false);
        }
    }

    UE_LOG(LogBuildingGen, Log,
        TEXT("RebuildHISM: %d buckets, %d instances"),
        HismBuckets.Num(), InstanceMap.Num());
}

// ─────────────────────────────────────────────────────────────────────
//  Single-tile edit (incremental AutoTile + full rebuild)
// ─────────────────────────────────────────────────────────────────────

void ABuildingActor::SetTile(int32 FloorIndex, int32 X, int32 Y, ETileType NewType)
{
    if (!BuildingData.Floors.IsValidIndex(FloorIndex))
        return;

    FFloorData& Floor = BuildingData.Floors[FloorIndex];
    if (!Floor.IsValidCoord(X, Y))
        return;

    Floor.GetTile(X, Y).TileType = NewType;

    auto ReprocessTile = [&](int32 TX, int32 TY)
    {
        if (!Floor.IsValidCoord(TX, TY)) return;
        FTileData& T = Floor.GetTile(TX, TY);
        if (T.IsWallFamily())
        {
            uint8 Mask = BuildingAutoTile::ComputeWallMask(Floor, TX, TY);
            BuildingAutoTile::LookupVariant(Mask, T.WallVariant, T.AutoRotationYaw);
        }
        else
        {
            T.WallVariant = EWallVariant::None;
            T.AutoRotationYaw = 0.f;
        }
    };

    ReprocessTile(X, Y);
    ReprocessTile(X, Y - 1);
    ReprocessTile(X, Y + 1);
    ReprocessTile(X + 1, Y);
    ReprocessTile(X - 1, Y);

    // v1.0: full rebuild for correctness; v2.0 can do incremental update
    RebuildHISM();
}

ETileType ABuildingActor::GetTileType(int32 FloorIndex, int32 X, int32 Y) const
{
    if (!BuildingData.Floors.IsValidIndex(FloorIndex))
        return ETileType::Empty;
    return BuildingData.Floors[FloorIndex].GetTileType(X, Y);
}

void ABuildingActor::ApplyPreset_OneStoryBuilding()
{
    constexpr int32 W = 10;
    constexpr int32 H = 8;

    using E = ETileType;

    // clang-format off
    const E F1[H][W] = {
     // x: 0      1      2      3      4      5      6      7      8      9
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall_Door,  E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y0: front + door
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y1
        { E::Wall_Window, E::Room_A, E::Room_A,  E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall_Window},  // y2: windows
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y3
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Corridor,   E::Corridor,   E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y4: divider
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Floor,      E::Floor,      E::Wall       },  // y5: hall
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Floor,      E::Floor,      E::Wall       },  // y6
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y7: back
    };
    // clang-format on

    BuildingData.Floors.Empty();
    BuildingData.TileSize = 400.f;
    BuildingData.FloorHeight = 300.f;

    FFloorData& Floor = BuildingData.AddFloor(W, H, TEXT("1F"));
    for (int32 Y = 0; Y < H; ++Y)
    {
        for (int32 X = 0; X < W; ++X)
        {
            Floor.SetTileType(X, Y, F1[Y][X]);
        }
    }

    BuildingAutoTile::ProcessBuilding(BuildingData);

    UE_LOG(LogBuildingGen, Log, TEXT("Applied preset: OneStoryBuilding (10x8, 1 floor)"));
}

void ABuildingActor::ApplyPreset_TwoStoryBuilding()
{
    constexpr int32 W = 10;
    constexpr int32 H = 8;

    using E = ETileType;

    // W=Wall, D=Wall_Door, N=Wall_Window, A=Room_A, B=Room_B, C=Corridor, F=Floor, U=Stairs_Up
    // clang-format off
    const E F1[H][W] = {
     // x: 0      1      2      3      4      5      6      7      8      9
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall_Door,  E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y0: front
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y1
        { E::Wall_Window, E::Room_A, E::Room_A,  E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall_Window},  // y2: windows
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y3
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Corridor,   E::Corridor,   E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y4: divider
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Stairs_Up,  E::Floor,      E::Wall       },  // y5: hall+stairs
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Floor,      E::Floor,      E::Wall       },  // y6
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y7: back
    };

    const E F2[H][W] = {
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y0: front (no door)
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y1
        { E::Wall_Window, E::Room_A, E::Room_A,  E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall_Window},  // y2: windows
        { E::Wall, E::Room_A,     E::Room_A,     E::Room_A,     E::Corridor,   E::Corridor,   E::Room_B,     E::Room_B,     E::Room_B,     E::Wall       },  // y3
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Corridor,   E::Corridor,   E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y4: divider
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Empty,      E::Floor,      E::Wall       },  // y5: stairwell opening at (7,5)
        { E::Wall, E::Floor,      E::Floor,      E::Floor,      E::Corridor,   E::Corridor,   E::Floor,      E::Floor,      E::Floor,      E::Wall       },  // y6
        { E::Wall, E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall,       E::Wall       },  // y7: back
    };
    // clang-format on

    BuildingData.Floors.Empty();
    BuildingData.TileSize = 400.f;
    BuildingData.FloorHeight = 300.f;

    auto ApplyGrid = [&](const FString& Name, const E Grid[H][W])
    {
        FFloorData& Floor = BuildingData.AddFloor(W, H, Name);
        for (int32 Y = 0; Y < H; ++Y)
        {
            for (int32 X = 0; X < W; ++X)
            {
                Floor.SetTileType(X, Y, Grid[Y][X]);
            }
        }
    };

    ApplyGrid(TEXT("1F"), F1);
    ApplyGrid(TEXT("2F"), F2);

    BuildingAutoTile::ProcessBuilding(BuildingData);

    UE_LOG(LogBuildingGen, Log, TEXT("Applied preset: TwoStoryBuilding (10x8, 2 floors)"));
}

// ─────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────

void ABuildingActor::BeginDestroy()
{
    DestroyAllEditProxies();
    Super::BeginDestroy();
}

void ABuildingActor::DestroyAllEditProxies()
{
    for (ABuildingModuleActor* Proxy : EditProxies)
    {
        if (IsValid(Proxy))
        {
            Proxy->Destroy();
        }
    }
    EditProxies.Empty();
}

#if WITH_EDITOR
void ABuildingActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropName = PropertyChangedEvent.GetMemberPropertyName();
    if (PropName == GET_MEMBER_NAME_CHECKED(ABuildingActor, BuildingData)
        || PropName == GET_MEMBER_NAME_CHECKED(ABuildingActor, MeshSet))
    {
        RebuildHISM();
    }
}
#endif
