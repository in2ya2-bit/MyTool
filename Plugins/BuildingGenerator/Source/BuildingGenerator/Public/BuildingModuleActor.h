#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BuildingTypes.h"
#include "BuildingModuleActor.generated.h"

class ABuildingActor;

/**
 * EditProxy — HISM 인스턴스를 숨기고 대신 배치되는 편집용 임시 액터.
 * RF_Transient: 레벨 저장 시 포함되지 않음.
 * 파괴 시 원본 HISM 인스턴스 자동 복원.
 */
UCLASS(NotPlaceable)
class BUILDINGGENERATOR_API ABuildingModuleActor : public AActor
{
    GENERATED_BODY()

public:
    ABuildingModuleActor();

    void Initialize(ABuildingActor* InOwner, int32 InFloorIndex, int32 InX, int32 InY);

    UPROPERTY(VisibleAnywhere, Category = "EditProxy")
    TObjectPtr<UStaticMeshComponent> MeshComponent;

    int32 GetFloorIndex() const { return FloorIndex; }
    int32 GetTileX() const { return TileX; }
    int32 GetTileY() const { return TileY; }
    ABuildingActor* GetOwnerBuilding() const { return OwnerBuilding.Get(); }

    void CommitEdit();
    void CancelEdit();

    virtual void BeginDestroy() override;

#if WITH_EDITOR
    virtual void PostEditMove(bool bFinished) override;
#endif

private:
    UPROPERTY()
    TWeakObjectPtr<ABuildingActor> OwnerBuilding;

    int32 FloorIndex = 0;
    int32 TileX = 0;
    int32 TileY = 0;
    bool bCommitted = false;
};
