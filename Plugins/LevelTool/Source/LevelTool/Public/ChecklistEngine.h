#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "ChecklistTypes.h"
#include "SuggestionTypes.h"
#include "ChecklistEngine.generated.h"

class UDesignerIntentSubsystem;
class UHeatmapGenerator;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCheckComplete, const FCheckReport& /*Report*/);

/**
 * UChecklistEngine
 *
 * 3단계 체크리스트 진단 엔진.
 * BR 10항목 / EX 10항목을 유효 맵 상태 기준으로 진단한다.
 */
UCLASS()
class LEVELTOOL_API UChecklistEngine : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UDesignerIntentSubsystem* InSubsystem);

	FCheckReport RunDiagnosis(ERulesetType Ruleset);
	const FCheckReport& GetLastReport() const { return LastReport; }

	// ── 제안 카드 ───────────────────────────────────────────────────────
	const TArray<FSuggestionCard>& GetSuggestions() const { return Suggestions; }
	bool AcceptSuggestion(const FString& CardId);
	bool RejectSuggestion(const FString& CardId);
	int32 AcceptAllSuggestions();
	int32 RejectAllSuggestions();

	// ── 대시보드 점수 ───────────────────────────────────────────────────
	float ComputeWeightedFitness(const FCheckReport& Report) const;
	struct FReferenceSimilarity { FString PresetName; float SimilarityPct; };
	TArray<FReferenceSimilarity> ComputeReferenceSimilarity() const;

	// ── 히트맵 ──────────────────────────────────────────────────────────
	UHeatmapGenerator* GetHeatmapGenerator() const { return HeatmapGen; }

	// ── 경로 그래프 분석 ────────────────────────────────────────────────
	struct FRoadEdge { int64 NodeA; int64 NodeB; float Weight; bool bIsBridge; };
	struct FRoadGraphResult
	{
		TMap<int64, TArray<int32>> AdjList;
		TArray<FRoadEdge> Edges;
		TArray<int32> BridgeEdgeIndices;
		int32 ComponentCount = 0;
	};
	FRoadGraphResult BuildRoadGraph() const;
	TArray<int32> FindBridgeEdges(const FRoadGraphResult& Graph) const;

	FOnCheckComplete OnCheckComplete;

	// ── BR/EX 가중치 (대시보드 외부 참조) ──────────────────────────────
	static float GetBRWeight(const FString& CheckId);
	static float GetEXWeight(const FString& CheckId);

private:
	// ── 거점 탐지 (DBSCAN) ──────────────────────────────────────────────
	TArray<FPoiCluster> DetectPois() const;

	// ── BR 체크 ─────────────────────────────────────────────────────────
	FCheckResult CheckBR1_PoiSpread(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckBR2_LootGradient(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckBR3_ChokeBypass() const;
	FCheckResult CheckBR4_OpenClosedMix() const;
	FCheckResult CheckBR5_CoverTheme() const;
	FCheckResult CheckBR6_VehicleRoute(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckBR7_ShrinkEquity() const;
	FCheckResult CheckBR8_SizeDensity() const;
	FCheckResult CheckBR9_VerticalCombat(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckBR10_ExploreReward() const;

	// ── EX 체크 ─────────────────────────────────────────────────────────
	FCheckResult CheckEX1_ExtractRoutes() const;
	FCheckResult CheckEX2_RiskReward(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckEX3_VerticalCombat(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckEX4_StealthRoute(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckEX5_ExtractTension() const;
	FCheckResult CheckEX6_RotationPaths(const TArray<FPoiCluster>& Pois) const;
	FCheckResult CheckEX7_SoloSquadCover() const;
	FCheckResult CheckEX8_LockedLoot() const;
	FCheckResult CheckEX9_NoiseTrigger() const;
	FCheckResult CheckEX10_AIZone() const;

	// ── 헬퍼 ────────────────────────────────────────────────────────────
	int32 CountMarkersByType(const FString& MarkerType) const;
	float ComputeBuildingCoveragePercent() const;

	// ── 제안 카드 생성 ──────────────────────────────────────────────────
	void GenerateSuggestions(const FCheckReport& Report, const TArray<FPoiCluster>& Pois);
	FSuggestionCard MakeMarkerSuggestion(const FString& CheckId, const FString& Problem,
		const FString& MarkerType, const FVector2D& Location, const FString& Reference) const;
	FSuggestionCard MakeBuildingRemoveSuggestion(const FString& CheckId, const FString& Problem,
		const FVector2D& Location, const FString& Reference) const;
	FSuggestionCard MakeRoadAddSuggestion(const FString& CheckId, const FString& Problem,
		const FVector2D& Start, const FVector2D& End, const FString& Reference) const;

	// P2-16: A* 경로 탐색 + Douglas-Peucker 단순화
	TArray<FVector2D> AStarPath(const FVector2D& Start, const FVector2D& End) const;

	// ── 4방향 접근성 검사 (BR-7) ────────────────────────────────────────
	int32 CountBlockedDirections(const FRoadGraphResult& Graph) const;

	UPROPERTY()
	TObjectPtr<UDesignerIntentSubsystem> Subsystem;

	UPROPERTY()
	TObjectPtr<UHeatmapGenerator> HeatmapGen;

	FCheckReport LastReport;
	TArray<FSuggestionCard> Suggestions;
	mutable int32 SuggestionCounter = 0;
	mutable FRoadGraphResult CachedRoadGraph;
	mutable bool bRoadGraphDirty = true;
};
