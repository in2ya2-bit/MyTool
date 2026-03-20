#pragma once

#include "CoreMinimal.h"
#include "DesignerIntentTypes.h"
#include "ChecklistTypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
//  체크 결과 상태
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class ECheckStatus : uint8
{
	Pass,
	Warning,
	Fail,
	NotApplicable   // 마커 미설정 등
};

// ─────────────────────────────────────────────────────────────────────────────
//  BR 체크 항목 (10항)
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class EBRCheck : uint8
{
	BR1_PoiSpread,
	BR2_LootGradient,
	BR3_ChokeBypass,
	BR4_OpenClosedMix,
	BR5_CoverTheme,
	BR6_VehicleRoute,
	BR7_ShrinkEquity,
	BR8_SizeDensity,
	BR9_VerticalCombat,
	BR10_ExploreReward,
	COUNT
};

// ─────────────────────────────────────────────────────────────────────────────
//  EX 체크 항목 (10항)
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class EEXCheck : uint8
{
	EX1_ExtractRoutes,
	EX2_RiskReward,
	EX3_VerticalCombat,
	EX4_StealthRoute,
	EX5_ExtractTension,
	EX6_RotationPaths,
	EX7_SoloSquadCover,
	EX8_LockedLoot,
	EX9_NoiseTrigger,
	EX10_AIZone,
	COUNT
};

// ─────────────────────────────────────────────────────────────────────────────
//  단일 체크 결과
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FCheckResult
{
	GENERATED_BODY()

	FString       CheckId;        // "BR-1", "EX-4" 등
	FString       Label;          // 한글 항목명
	ECheckStatus  Status    = ECheckStatus::NotApplicable;
	float         Score     = 0.f;  // 감점/가점 (-15 ~ +5)
	FString       Detail;         // "S등급 간 거리 350m (기준 400m)"
	FString       Suggestion;     // "거점 A와 B가 너무 가깝습니다"
	FVector       FocusLocation = FVector::ZeroVector;
	TArray<FString> HighlightActors;
};

// ─────────────────────────────────────────────────────────────────────────────
//  거점 정보 (DBSCAN 결과)
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FPoiCluster
{
	GENERATED_BODY()

	FString   Label;           // 자동 생성: "거점_A", "거점_B"
	FString   Grade;           // "S", "A", "B", "C"
	FVector2D Centroid = FVector2D::ZeroVector;
	float     Score    = 0.f;
	int32     BuildingCount = 0;
	float     AreaM2   = 0.f;
	float     MaxHeightM = 0.f;
	TArray<FVector2D> BoundaryPoints;  // Convex Hull
};

// ─────────────────────────────────────────────────────────────────────────────
//  전체 보고서
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FCheckReport
{
	GENERATED_BODY()

	ERulesetType    Ruleset = ERulesetType::BR;
	TArray<FCheckResult> Results;
	TArray<FPoiCluster>  DetectedPois;
	float           TotalScore = 100.f;   // 100에서 감점 방식
	FString         MapId;
	FDateTime       Timestamp;

	int32 CountByStatus(ECheckStatus S) const
	{
		int32 N = 0;
		for (const auto& R : Results) if (R.Status == S) N++;
		return N;
	}
};
