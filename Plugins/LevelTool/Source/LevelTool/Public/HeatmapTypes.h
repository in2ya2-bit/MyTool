#pragma once

#include "CoreMinimal.h"
#include "HeatmapTypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
//  히트맵 모드
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class EHeatmapMode : uint8
{
	BuildingDensity,    // 건물 밀도  (빨강 과밀 ~ 초록 적정)
	RoadConnectivity,   // 도로 연결성 (빨강 단절 ~ 초록 2+ 경로)
	PoiDistribution,    // 거점 분포  (마커 위치 + 반경)
	Elevation,          // 고저차    (등고선 오버레이)
	COUNT
};

// ─────────────────────────────────────────────────────────────────────────────
//  히트맵 셀
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FHeatmapCell
{
	GENERATED_BODY()

	int32 GridX = 0;
	int32 GridY = 0;
	FVector2D WorldCenter = FVector2D::ZeroVector;
	float Value = 0.f;    // 0~1 정규화
	FLinearColor Color = FLinearColor::Black;
};

// ─────────────────────────────────────────────────────────────────────────────
//  히트맵 데이터
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FHeatmapData
{
	GENERATED_BODY()

	EHeatmapMode     Mode = EHeatmapMode::BuildingDensity;
	int32            GridResolution = 0;
	float            CellSizeCm    = 5000.f;   // 50m
	TArray<FHeatmapCell> Cells;

	float MinValue = 0.f;
	float MaxValue = 1.f;
	FString Label;

	const FHeatmapCell* FindCell(int32 GX, int32 GY) const
	{
		for (const auto& C : Cells)
		{
			if (C.GridX == GX && C.GridY == GY) return &C;
		}
		return nullptr;
	}
};
