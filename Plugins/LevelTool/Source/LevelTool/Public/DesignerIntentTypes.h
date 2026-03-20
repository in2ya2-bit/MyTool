#pragma once

#include "CoreMinimal.h"
#include "DesignerIntentTypes.generated.h"

UENUM()
enum class ESliderType : uint8
{
	UrbanDensity,
	Openness,
	RouteComplexity,
	ElevationContrast,
	DestructionLevel,
};

UENUM()
enum class ERulesetType : uint8
{
	BR,
	Extraction,
};

UENUM()
enum class ESingleEditType : uint8
{
	Remove,
	HeightChange,
	DestructionChange,
	AddMarker,
};

UENUM()
enum class EPresetSelectionMode : uint8
{
	None,
	Building,
	Area,
	Road,
};

UENUM()
enum class EObjectPresetType : uint8
{
	SniperPoint,       // 고지대 저격 포인트
	CentralPlaza,      // 중앙 광장 (개방 + 주변 엄폐)
	NarrowEntry,       // 좁은 진입로 (커버 + 병목)
	ChokepointBypass,  // 병목 + 우회로
	VerticalCombat,    // 수직 전투 구역 (다층 건물)
};

struct FSliderState
{
	float InitialValue       = 0.f;
	float CurrentValue       = 0.f;
	float AchievementRate    = 100.f;
	float AchievementLimitMax = 100.f;
	FString LimitReason;
};

struct FRoadGraphNode
{
	int64     CellKey = 0;
	FVector2D Location = FVector2D::ZeroVector;
	int32     Degree   = 0;
};

struct FRoadGraphEdge
{
	int64 NodeA = 0;
	int64 NodeB = 0;
	float LengthCm = 0.f;
	bool  bIsBridge = false;
};

struct FEffectiveRoadGraph
{
	TMap<int64, FRoadGraphNode> Nodes;
	TArray<FRoadGraphEdge>      Edges;
	int32 ConnectedComponents    = 0;
	int32 BridgeEdgeCount        = 0;

	void BuildFromRoads(const TArray<TArray<FVector2D>>& RoadPolylines, float CellSizeCm = 200.f);
	TArray<int64> FindBridgeEdgeNodePairs() const;
	float AverageNodeDegree() const;
};
