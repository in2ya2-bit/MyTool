#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "BuildingTypes.h"
#include "BuildingDataAsset.generated.h"

UCLASS(BlueprintType)
class BUILDINGGENERATOR_API UBuildingDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walls")
    TMap<EWallVariant, FMeshSlot> WallMeshes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walls")
    FMeshSlot DoorMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walls")
    FMeshSlot WindowMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floors")
    TMap<ETileType, FMeshSlot> FloorMeshes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stairs")
    FMeshSlot StairsUpMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stairs")
    FMeshSlot StairsDownMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
    FMeshSlot CeilingMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fallback")
    FMeshSlot FallbackMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings",
        meta = (ContentDir))
    FDirectoryPath MeshSourceDirectory;

    const FMeshSlot* ResolveMeshSlot(ETileType TileType, EWallVariant Variant = EWallVariant::None) const;

    TArray<FString> Validate() const;

    UFUNCTION(BlueprintCallable, Category = "Building")
    int32 AutoPopulateFromDirectory(const FString& ContentPath = TEXT("/Game/BuildingGenerator/Meshes"));
};
