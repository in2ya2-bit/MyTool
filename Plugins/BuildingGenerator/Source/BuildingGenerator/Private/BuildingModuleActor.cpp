#include "BuildingModuleActor.h"
#include "BuildingActor.h"
#include "BuildingDataAsset.h"

ABuildingModuleActor::ABuildingModuleActor()
{
    PrimaryActorTick.bCanEverTick = false;

    MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
    RootComponent = MeshComponent;

    SetFlags(RF_Transient);
}

void ABuildingModuleActor::Initialize(
    ABuildingActor* InOwner, int32 InFloorIndex, int32 InX, int32 InY)
{
    OwnerBuilding = InOwner;
    FloorIndex = InFloorIndex;
    TileX = InX;
    TileY = InY;

    if (!InOwner || !InOwner->MeshSet)
        return;

    if (!InOwner->BuildingData.Floors.IsValidIndex(InFloorIndex))
        return;

    const FFloorData& Floor = InOwner->BuildingData.Floors[InFloorIndex];
    if (!Floor.IsValidCoord(InX, InY))
        return;

    const FTileData& Tile = Floor.GetTile(InX, InY);
    const FMeshSlot* Slot = InOwner->MeshSet->ResolveMeshSlot(Tile.TileType, Tile.WallVariant);

    if (Slot)
    {
        MeshComponent->SetStaticMesh(Slot->LoadMesh());
    }

    FVector Location = InOwner->BuildingData.TileToWorld(InFloorIndex, InX, InY);
    if (Slot) Location += Slot->PivotOffset;
    FRotator Rotation(0.f, Tile.AutoRotationYaw, 0.f);
    FVector Scale = Slot ? Slot->ScaleCorrection : FVector::OneVector;

    SetActorTransform(FTransform(Rotation,
        InOwner->GetActorLocation() + Location, Scale));

    InOwner->HideInstance(InFloorIndex, InX, InY);
    InOwner->EditProxies.Add(this);

    SetActorLabel(FString::Printf(TEXT("EditProxy_%d_%d_%d"), InFloorIndex, InX, InY));
}

void ABuildingModuleActor::CommitEdit()
{
    bCommitted = true;

    ABuildingActor* OwningActor = OwnerBuilding.Get();
    if (!OwningActor) return;

    OwningActor->RestoreInstance(FloorIndex, TileX, TileY);
    OwningActor->EditProxies.Remove(this);
}

void ABuildingModuleActor::CancelEdit()
{
    ABuildingActor* OwningActor = OwnerBuilding.Get();
    if (OwningActor)
    {
        OwningActor->RestoreInstance(FloorIndex, TileX, TileY);
        OwningActor->EditProxies.Remove(this);
    }
    Destroy();
}

void ABuildingModuleActor::BeginDestroy()
{
    if (!bCommitted)
    {
        ABuildingActor* OwningActor = OwnerBuilding.Get();
        if (OwningActor)
        {
            OwningActor->RestoreInstance(FloorIndex, TileX, TileY);
            OwningActor->EditProxies.Remove(this);
        }
    }

    Super::BeginDestroy();
}

#if WITH_EDITOR
void ABuildingModuleActor::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);
}
#endif
