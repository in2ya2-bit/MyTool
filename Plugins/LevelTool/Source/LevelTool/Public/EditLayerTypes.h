#pragma once

#include "CoreMinimal.h"
#include "EditLayerTypes.generated.h"

class FJsonObject;

// ─────────────────────────────────────────────────────────────────────────────
//  Enums
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class EEditLayerType : uint8
{
	TerrainModify,
	BuildingAdd,
	BuildingRemove,
	BuildingHeight,
	RoadBlock,
	RoadAdd,
	RoadWidth,
	DestructionState,
	Marker,
	BuildingAddBatch,
};

UENUM()
enum class EAreaType : uint8
{
	Polygon,
	Circle,
	Point,
	Path,
	ActorRef,
};

UENUM()
enum class ELayerCreatedBy : uint8
{
	Slider,
	Preset,
	ReferencePreset,
	AiSuggest,
	Manual,
};

UENUM()
enum class EResolveStatus : uint8
{
	Direct,
	Fallback,
	Missing,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Area data variants
// ─────────────────────────────────────────────────────────────────────────────

struct FAreaPolygon
{
	TArray<FVector2D> PointsUE5;
};

struct FAreaCircle
{
	FVector2D CenterUE5 = FVector2D::ZeroVector;
	float RadiusCm = 0.f;
};

struct FAreaPoint
{
	FVector LocationUE5 = FVector::ZeroVector;
};

struct FAreaPath
{
	TArray<FVector2D> PointsUE5;
	float WidthCm = 600.f;
};

struct FAreaActorRef
{
	FString StableId;
	FVector FallbackLocationUE5 = FVector::ZeroVector;
};

struct FEditLayerArea
{
	EAreaType Type = EAreaType::Point;

	FAreaPolygon  Polygon;
	FAreaCircle   Circle;
	FAreaPoint    Point;
	FAreaPath     Path;
	FAreaActorRef ActorRef;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Edit Layer
// ─────────────────────────────────────────────────────────────────────────────

struct FEditLayer
{
	FString            LayerId;
	EEditLayerType     Type       = EEditLayerType::Marker;
	FEditLayerArea     Area;
	TSharedPtr<FJsonObject> Params;
	FString            Label;
	bool               bLocked    = false;
	bool               bVisible   = true;
	ELayerCreatedBy    CreatedBy  = ELayerCreatedBy::Manual;
	FString            SourceId;
	int32              Generation = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Stable ID resolve result (for future Apply logic)
// ─────────────────────────────────────────────────────────────────────────────

struct FResolveResult
{
	TWeakObjectPtr<AActor> Actor;
	EResolveStatus Status = EResolveStatus::Missing;
};
