#include "BuildingAutoTile.h"

namespace
{
    struct FAutoTileEntry
    {
        EWallVariant Variant;
        float Yaw;
    };

    // 16-entry lookup: N=bit0 S=bit1 E=bit2 W=bit3
    static const FAutoTileEntry GAutoTileTable[16] =
    {
        { EWallVariant::Isolated,     0.f },   // 0000 ----
        { EWallVariant::End,          0.f },   // 0001 N
        { EWallVariant::End,        180.f },   // 0010 S
        { EWallVariant::Straight,     0.f },   // 0011 N+S
        { EWallVariant::End,         90.f },   // 0100 E
        { EWallVariant::Corner,       0.f },   // 0101 N+E
        { EWallVariant::Corner,      90.f },   // 0110 S+E
        { EWallVariant::T_Junction,   0.f },   // 0111 N+S+E
        { EWallVariant::End,        270.f },   // 1000 W
        { EWallVariant::Corner,     270.f },   // 1001 N+W
        { EWallVariant::Corner,     180.f },   // 1010 S+W
        { EWallVariant::T_Junction, 180.f },   // 1011 N+S+W
        { EWallVariant::Straight,    90.f },   // 1100 E+W
        { EWallVariant::T_Junction, 270.f },   // 1101 N+E+W
        { EWallVariant::T_Junction,  90.f },   // 1110 S+E+W
        { EWallVariant::Cross,        0.f },   // 1111 N+S+E+W
    };
}

namespace BuildingAutoTile
{
    uint8 ComputeWallMask(const FFloorData& Floor, int32 X, int32 Y)
    {
        uint8 Mask = 0;

        auto IsWall = [&](int32 TX, int32 TY) -> bool
        {
            if (!Floor.IsValidCoord(TX, TY)) return false;
            return Floor.GetTile(TX, TY).IsWallFamily();
        };

        if (IsWall(X, Y - 1)) Mask |= 0b0001;   // N
        if (IsWall(X, Y + 1)) Mask |= 0b0010;   // S
        if (IsWall(X + 1, Y)) Mask |= 0b0100;   // E
        if (IsWall(X - 1, Y)) Mask |= 0b1000;   // W

        return Mask;
    }

    void LookupVariant(uint8 Mask, EWallVariant& OutVariant, float& OutYaw)
    {
        const FAutoTileEntry& Entry = GAutoTileTable[Mask & 0x0F];
        OutVariant = Entry.Variant;
        OutYaw = Entry.Yaw;
    }

    void ProcessFloor(FFloorData& Floor)
    {
        for (int32 Y = 0; Y < Floor.Height; ++Y)
        {
            for (int32 X = 0; X < Floor.Width; ++X)
            {
                FTileData& Tile = Floor.GetTile(X, Y);
                if (!Tile.IsWallFamily())
                {
                    Tile.WallVariant = EWallVariant::None;
                    if (!Tile.IsStairs())
                    {
                        Tile.AutoRotationYaw = 0.f;
                    }
                    continue;
                }

                uint8 Mask = ComputeWallMask(Floor, X, Y);
                LookupVariant(Mask, Tile.WallVariant, Tile.AutoRotationYaw);
            }
        }
    }

    void ProcessBuilding(FBuildingData& Building)
    {
        for (FFloorData& Floor : Building.Floors)
        {
            ProcessFloor(Floor);
        }
    }
}
