#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "BuildingTypes.h"
#include "BuildingActor.generated.h"

class UBuildingDataAsset;
class ABuildingModuleActor;

USTRUCT()
struct FBuildingInstance
{
    GENERATED_BODY()

    UPROPERTY()
    int32 FloorIndex = 0;

    UPROPERTY()
    int32 TileX = 0;

    UPROPERTY()
    int32 TileY = 0;

    UPROPERTY()
    int32 HismInstanceIndex = -1;

    UPROPERTY()
    FString BucketKey;

    UPROPERTY()
    bool bHidden = false;
};

UCLASS()
class BUILDINGGENERATOR_API ABuildingActor : public AActor
{
    GENERATED_BODY()

public:
    ABuildingActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building",
        meta = (ShowOnlyInnerProperties))
    FBuildingData BuildingData;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
    TObjectPtr<UBuildingDataAsset> MeshSet;

    UFUNCTION(BlueprintCallable, Category = "Building")
    void RebuildHISM();

    UFUNCTION(BlueprintCallable, Category = "Building")
    void SetTile(int32 FloorIndex, int32 X, int32 Y, ETileType NewType);

    UFUNCTION(BlueprintCallable, Category = "Building")
    ETileType GetTileType(int32 FloorIndex, int32 X, int32 Y) const;

    UFUNCTION(BlueprintCallable, Category = "Building")
    void ApplyPreset_OneStoryBuilding();

    UFUNCTION(BlueprintCallable, Category = "Building")
    void ApplyPreset_TwoStoryBuilding();

    void HideInstance(int32 FloorIndex, int32 X, int32 Y);
    void RestoreInstance(int32 FloorIndex, int32 X, int32 Y);
    void RepairHiddenInstances();

    virtual void BeginDestroy() override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    UPROPERTY()
    TMap<FString, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HismBuckets;

    UPROPERTY()
    TMap<FString, FBuildingInstance> InstanceMap;

    UPROPERTY(Transient)
    TArray<TObjectPtr<ABuildingModuleActor>> EditProxies;

    float CachedMeshScale = 1.f;

    void ComputeCachedMeshScale();
    FVector ComputeTilePivot() const;

    UHierarchicalInstancedStaticMeshComponent* GetOrCreateBucket(
        const FString& MeshIdentity, UStaticMesh* Mesh);
    FString MakeInstanceKey(int32 FloorIndex, int32 X, int32 Y) const;
    void AddTileInstance(int32 FloorIndex, int32 X, int32 Y, const FTileData& Tile);
    void AddBaseFloorInstance(int32 FloorIndex, int32 X, int32 Y);
    void AddCeilingInstance(int32 FloorIndex, int32 X, int32 Y);
    void AutoApplyStairFlags();
    void ClearAllBuckets();
    void DestroyAllEditProxies();

    friend class ABuildingModuleActor;
};
