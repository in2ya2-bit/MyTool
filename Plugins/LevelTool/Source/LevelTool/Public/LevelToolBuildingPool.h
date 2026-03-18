#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "LevelToolBuildingPool.generated.h"

// ---------------------------------------------------------------------------
//  One entry in the mesh pool — a building category + its mesh + materials
// ---------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FBuildingMeshEntry
{
    GENERATED_BODY()

    // OSM building type string that maps to this entry
    // e.g. "BP_Building_Residential", "BP_Building_Commercial"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
    FString TypeKey;

    // Display name shown in editor UI
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
    FString DisplayName;

    // Base mesh (1m tall, UE5 will scale Z to match building height)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    TSoftObjectPtr<UStaticMesh> Mesh;

    // Optional mesh variants (randomly picked per building instance)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh",
        meta = (ToolTip = "Randomly selected when placing. Leave empty to always use Mesh."))
    TArray<TSoftObjectPtr<UStaticMesh>> MeshVariants;

    // Material override (if null, uses mesh default)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    TSoftObjectPtr<UMaterialInterface> WallMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    TSoftObjectPtr<UMaterialInterface> RoofMaterial;

    // Placement rules
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement",
        meta = (ToolTip = "Random height variation multiplier range (min, max)"))
    FVector2D HeightVariationRange = FVector2D(0.9f, 1.1f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement",
        meta = (ToolTip = "Rotate to align with nearest road (requires road data)"))
    bool bAlignToRoad = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement",
        meta = (ToolTip = "Generate LODs using Nanite when importing FBX"))
    bool bEnableNanite = true;

    // Collision preset name
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
    FName CollisionProfile = TEXT("BlockAll");
};

// ---------------------------------------------------------------------------
//  The DataAsset — lives in /Game/LevelTool/Data/DA_BuildingPool
// ---------------------------------------------------------------------------
UCLASS(BlueprintType)
class LEVELTOOL_API ULevelToolBuildingPool : public UDataAsset
{
    GENERATED_BODY()

public:
    // All building type entries
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Pool")
    TArray<FBuildingMeshEntry> Entries;

    // Fallback mesh used when no matching TypeKey is found
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Pool")
    TSoftObjectPtr<UStaticMesh> FallbackMesh;

    // Default materials applied to all buildings without specific overrides
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Pool")
    TSoftObjectPtr<UMaterialInterface> DefaultWallMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Pool")
    TSoftObjectPtr<UMaterialInterface> DefaultRoofMaterial;

    // PCG graph for scattering props on buildings (rooftop clutter, antennas etc.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PCG Integration")
    TSoftObjectPtr<UObject> RooftopPCGGraph;

    // ── Query Helpers ────────────────────────────────────────────────────

    // Find entry by type key, returns nullptr if not found
    const FBuildingMeshEntry* FindEntry(const FString& TypeKey) const
    {
        for (const FBuildingMeshEntry& Entry : Entries)
        {
            if (Entry.TypeKey == TypeKey)
                return &Entry;
        }
        return nullptr;
    }

    // Get mesh for a type key, falling back to FallbackMesh
    UStaticMesh* ResolveMesh(const FString& TypeKey) const
    {
        if (const FBuildingMeshEntry* Entry = FindEntry(TypeKey))
        {
            // Pick variant if available
            if (!Entry->MeshVariants.IsEmpty())
            {
                int32 Idx = FMath::RandRange(0, Entry->MeshVariants.Num() - 1);
                if (UStaticMesh* VarMesh = Entry->MeshVariants[Idx].LoadSynchronous())
                    return VarMesh;
            }
            if (UStaticMesh* M = Entry->Mesh.LoadSynchronous())
                return M;
        }
        return FallbackMesh.LoadSynchronous();
    }

    // Get all type keys (for editor dropdowns)
    TArray<FString> GetAllTypeKeys() const
    {
        TArray<FString> Keys;
        for (const FBuildingMeshEntry& Entry : Entries)
            Keys.Add(Entry.TypeKey);
        return Keys;
    }

    // Validate — called in editor to warn about missing meshes
    TArray<FString> Validate() const
    {
        TArray<FString> Warnings;
        for (const FBuildingMeshEntry& Entry : Entries)
        {
            if (Entry.TypeKey.IsEmpty())
                Warnings.Add(TEXT("Entry has empty TypeKey"));
            if (Entry.Mesh.IsNull())
                Warnings.Add(FString::Printf(TEXT("'%s' has no mesh assigned"), *Entry.TypeKey));
        }
        if (FallbackMesh.IsNull())
            Warnings.Add(TEXT("No FallbackMesh assigned — unmatched buildings will be invisible"));
        return Warnings;
    }
};
