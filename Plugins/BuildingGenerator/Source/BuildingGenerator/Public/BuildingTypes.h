#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "BuildingTypes.generated.h"

// ═══════════════════════════════════════════════════════
// ETileType — 타일 종류 (구현서 03장)
// ═══════════════════════════════════════════════════════
UENUM(BlueprintType)
enum class ETileType : uint8
{
    Empty       = 0,
    Floor       = 1,
    Wall        = 2,
    Wall_Door   = 3,
    Wall_Window = 4,
    Stairs      = 5,
    Room_A      = 6,
    Room_B      = 7,
    Room_C      = 8,
    Corridor    = 9,
    Stairs_Up   = 10,
    Stairs_Down = 11,
    MAX         UMETA(Hidden)
};

// ═══════════════════════════════════════════════════════
// EWallVariant — AutoTile 결과 변형 (구현서 04장)
// N=0001 S=0010 E=0100 W=1000, 16가지 조합 → 6 변형
// ═══════════════════════════════════════════════════════
UENUM(BlueprintType)
enum class EWallVariant : uint8
{
    None       = 0,
    Isolated   = 1,    // 0000: 독립
    Straight   = 2,    // 0011, 1100: 직선
    Corner     = 3,    // 0101, 0110, 1001, 1010: 코너
    T_Junction = 4,    // 0111, 1011, 1101, 1110: T자
    Cross      = 5,    // 1111: 십자
    End        = 6,    // 0001, 0010, 0100, 1000: 끝
    MAX        UMETA(Hidden)
};

// ═══════════════════════════════════════════════════════
// FMeshSlot — 메시 참조 + ScaleCorrection / PivotOffset
// ═══════════════════════════════════════════════════════
USTRUCT(BlueprintType)
struct BUILDINGGENERATOR_API FMeshSlot
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    TSoftObjectPtr<UStaticMesh> Mesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    FVector ScaleCorrection = FVector(1.f, 1.f, 1.f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
    FVector PivotOffset = FVector::ZeroVector;

    UStaticMesh* LoadMesh() const { return Mesh.LoadSynchronous(); }
    bool IsValid() const { return !Mesh.IsNull(); }
};

// ═══════════════════════════════════════════════════════
// FTileData — 단일 타일
// ═══════════════════════════════════════════════════════
USTRUCT(BlueprintType)
struct BUILDINGGENERATOR_API FTileData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tile")
    ETileType TileType = ETileType::Empty;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AutoTile")
    EWallVariant WallVariant = EWallVariant::None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AutoTile")
    float AutoRotationYaw = 0.f;

    bool IsWallFamily() const
    {
        return TileType == ETileType::Wall
            || TileType == ETileType::Wall_Door
            || TileType == ETileType::Wall_Window;
    }

    bool IsFloorFamily() const
    {
        return TileType == ETileType::Floor
            || TileType == ETileType::Room_A
            || TileType == ETileType::Room_B
            || TileType == ETileType::Room_C
            || TileType == ETileType::Corridor;
    }

    bool IsStairs() const
    {
        return TileType == ETileType::Stairs
            || TileType == ETileType::Stairs_Up
            || TileType == ETileType::Stairs_Down;
    }

    bool IsEmpty() const { return TileType == ETileType::Empty; }
};

// ═══════════════════════════════════════════════════════
// FFloorData — 1개 층 (Width × Height 타일 그리드)
// ═══════════════════════════════════════════════════════
USTRUCT(BlueprintType)
struct BUILDINGGENERATOR_API FFloorData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floor")
    int32 Width = 14;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floor")
    int32 Height = 10;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floor")
    FString FloorName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Floor")
    TArray<FTileData> Tiles;

    void Initialize(int32 InWidth, int32 InHeight)
    {
        Width = FMath::Max(1, InWidth);
        Height = FMath::Max(1, InHeight);
        Tiles.SetNum(Width * Height);
    }

    void EnsureTilesSize()
    {
        Width = FMath::Max(1, Width);
        Height = FMath::Max(1, Height);
        const int32 Required = Width * Height;
        if (Tiles.Num() == Required)
            return;

        TArray<FTileData> OldTiles = MoveTemp(Tiles);
        Tiles.SetNum(Required);

        if (OldTiles.Num() == 0)
            return;

        int32 OldW = Width;
        int32 OldH = OldTiles.Num() / FMath::Max(1, Width);
        if (OldW * OldH != OldTiles.Num())
        {
            OldW = OldTiles.Num() / FMath::Max(1, Height);
            OldH = Height;
        }
        if (OldW * OldH != OldTiles.Num())
            return;

        const int32 CopyW = FMath::Min(Width, OldW);
        const int32 CopyH = FMath::Min(Height, OldH);
        for (int32 Y = 0; Y < CopyH; ++Y)
        {
            for (int32 X = 0; X < CopyW; ++X)
            {
                Tiles[Y * Width + X] = OldTiles[Y * OldW + X];
            }
        }
    }

    bool IsValidCoord(int32 X, int32 Y) const
    {
        return X >= 0 && X < Width && Y >= 0 && Y < Height
            && (Y * Width + X) < Tiles.Num();
    }

    int32 CoordToIndex(int32 X, int32 Y) const
    {
        return Y * Width + X;
    }

    FTileData& GetTile(int32 X, int32 Y)
    {
        check(IsValidCoord(X, Y));
        return Tiles[CoordToIndex(X, Y)];
    }

    const FTileData& GetTile(int32 X, int32 Y) const
    {
        check(IsValidCoord(X, Y));
        return Tiles[CoordToIndex(X, Y)];
    }

    ETileType GetTileType(int32 X, int32 Y) const
    {
        if (!IsValidCoord(X, Y)) return ETileType::Empty;
        return Tiles[CoordToIndex(X, Y)].TileType;
    }

    void SetTileType(int32 X, int32 Y, ETileType Type)
    {
        if (IsValidCoord(X, Y))
        {
            Tiles[CoordToIndex(X, Y)].TileType = Type;
        }
    }
};

// ═══════════════════════════════════════════════════════
// FBuildingData — 건물 전체 (다층 구조)
// ═══════════════════════════════════════════════════════
USTRUCT(BlueprintType)
struct BUILDINGGENERATOR_API FBuildingData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
    TArray<FFloorData> Floors;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building",
        meta = (ClampMin = "100.0", UIMin = "100.0"))
    float TileSize = 400.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building",
        meta = (ClampMin = "100.0", UIMin = "100.0"))
    float FloorHeight = 300.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building",
        meta = (ClampMin = "0.1", UIMin = "0.1", ClampMax = "10.0", UIMax = "10.0"))
    float BuildingScale = 1.f;

    int32 GetFloorCount() const { return Floors.Num(); }

    FFloorData& AddFloor(int32 InWidth, int32 InHeight, const FString& Name)
    {
        FFloorData& Floor = Floors.AddDefaulted_GetRef();
        Floor.Initialize(InWidth, InHeight);
        Floor.FloorName = Name;
        return Floor;
    }

    FVector TileToWorld(int32 FloorIndex, int32 X, int32 Y) const
    {
        return FVector(
            X * TileSize,
            Y * TileSize,
            FloorIndex * FloorHeight
        );
    }
};
