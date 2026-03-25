#pragma once

#include "CoreMinimal.h"
#include "BuildingTypes.h"

/**
 * AutoTile — 4-bit bitmask (N=0001 S=0010 E=0100 W=1000) 기반
 * 16가지 이웃 조합 → EWallVariant + AutoRotationYaw 결정
 */
namespace BuildingAutoTile
{
    /** 한 층 전체 AutoTile 처리: 벽 계열 타일의 WallVariant/AutoRotationYaw 갱신 */
    void ProcessFloor(FFloorData& Floor);

    /** 건물 전체(모든 층) AutoTile 처리 */
    void ProcessBuilding(FBuildingData& Building);

    /** (X,Y) 타일의 4-bit 이웃 마스크 계산 */
    uint8 ComputeWallMask(const FFloorData& Floor, int32 X, int32 Y);

    /** 마스크 → 변형 + 회전 룩업 */
    void LookupVariant(uint8 Mask, EWallVariant& OutVariant, float& OutYaw);
}
