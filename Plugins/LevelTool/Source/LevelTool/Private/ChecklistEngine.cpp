#include "ChecklistEngine.h"
#include "DesignerIntentSubsystem.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"
#include "HeatmapGenerator.h"
#include "PresetTypes.h"

#include "LevelToolPerfGuard.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Misc/Guid.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogChecklist, Log, All);

static const int32 kDBSCAN_MinPts   = 5;

// ─────────────────────────────────────────────────────────────────────────────
void UChecklistEngine::Initialize(UDesignerIntentSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
	HeatmapGen = NewObject<UHeatmapGenerator>(this);
	HeatmapGen->Initialize(InSubsystem);
}

// ─────────────────────────────────────────────────────────────────────────────
FCheckReport UChecklistEngine::RunDiagnosis(ERulesetType Ruleset)
{
	LEVELTOOL_SCOPED_TIMER(CheckDiagnosis);

	if (Subsystem)
	{
		Subsystem->RebuildEffectiveState();
	}

	FCheckReport Report;
	Report.Ruleset   = Ruleset;
	Report.MapId     = Subsystem ? Subsystem->GetActiveMapId() : TEXT("");
	Report.Timestamp = FDateTime::UtcNow();
	Report.TotalScore = 100.f;

	bRoadGraphDirty = true;

	TArray<FPoiCluster> Pois = DetectPois();
	Report.DetectedPois = Pois;

	if (Ruleset == ERulesetType::BR)
	{
		Report.Results.Add(CheckBR1_PoiSpread(Pois));
		Report.Results.Add(CheckBR2_LootGradient(Pois));
		Report.Results.Add(CheckBR3_ChokeBypass());
		Report.Results.Add(CheckBR4_OpenClosedMix());
		Report.Results.Add(CheckBR5_CoverTheme());
		Report.Results.Add(CheckBR6_VehicleRoute(Pois));
		Report.Results.Add(CheckBR7_ShrinkEquity());
		Report.Results.Add(CheckBR8_SizeDensity());
		Report.Results.Add(CheckBR9_VerticalCombat(Pois));
		Report.Results.Add(CheckBR10_ExploreReward());
	}
	else
	{
		Report.Results.Add(CheckEX1_ExtractRoutes());
		Report.Results.Add(CheckEX2_RiskReward(Pois));
		Report.Results.Add(CheckEX3_VerticalCombat(Pois));
		Report.Results.Add(CheckEX4_StealthRoute(Pois));
		Report.Results.Add(CheckEX5_ExtractTension());
		Report.Results.Add(CheckEX6_RotationPaths(Pois));
		Report.Results.Add(CheckEX7_SoloSquadCover());
		Report.Results.Add(CheckEX8_LockedLoot());
		Report.Results.Add(CheckEX9_NoiseTrigger());
		Report.Results.Add(CheckEX10_AIZone());
	}

	Report.TotalScore = ComputeWeightedFitness(Report);

	LastReport = Report;

	GenerateSuggestions(Report, Pois);

	OnCheckComplete.Broadcast(Report);

	if (HeatmapGen)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakHM = TWeakObjectPtr<UHeatmapGenerator>(HeatmapGen)]()
		{
			if (UHeatmapGenerator* HM = WeakHM.Get())
			{
				HM->GenerateAll();
			}
		});
	}

	UE_LOG(LogChecklist, Log, TEXT("진단 완료: %s — 적합도 %.0f (P:%d W:%d F:%d, 제안 %d건)"),
		Ruleset == ERulesetType::BR ? TEXT("BR") : TEXT("EX"),
		Report.TotalScore,
		Report.CountByStatus(ECheckStatus::Pass),
		Report.CountByStatus(ECheckStatus::Warning),
		Report.CountByStatus(ECheckStatus::Fail),
		Suggestions.Num());

	return Report;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DBSCAN 기반 거점 탐지
// ─────────────────────────────────────────────────────────────────────────────

TArray<FPoiCluster> UChecklistEngine::DetectPois() const
{
	LEVELTOOL_SCOPED_TIMER(DBSCAN);
	TArray<FPoiCluster> Result;
	if (!Subsystem) return Result;

	struct FPt { FVector2D Pos; float HeightM; float AreaM2; int32 ClusterId = -1; };
	TArray<FPt> Points;
	Points.Reserve(Subsystem->EffectiveBuildings.Num());
	for (const auto& B : Subsystem->EffectiveBuildings)
	{
		Points.Add({ B.CentroidUE5, B.HeightM, B.AreaM2, -1 });
	}
	if (Points.Num() < kDBSCAN_MinPts) return Result;

	const float EpsCm = Subsystem ? Subsystem->GetScaledDBSCAN_Eps() : 15000.f;
	const float EpsSq = EpsCm * EpsCm;

	// Spatial hash grid for O(n) average neighbor queries instead of O(n²)
	const float CellSize = EpsCm;
	auto CellKey = [CellSize](const FVector2D& P) -> int64 {
		int32 cx = FMath::FloorToInt(P.X / CellSize);
		int32 cy = FMath::FloorToInt(P.Y / CellSize);
		return (int64(cx) << 32) | int64(uint32(cy));
	};
	TMultiMap<int64, int32> Grid;
	Grid.Reserve(Points.Num());
	for (int32 i = 0; i < Points.Num(); ++i)
		Grid.Add(CellKey(Points[i].Pos), i);

	auto FindNeighbors = [&](int32 Idx) -> TArray<int32> {
		TArray<int32> Nbrs;
		const FVector2D& P = Points[Idx].Pos;
		int32 cx = FMath::FloorToInt(P.X / CellSize);
		int32 cy = FMath::FloorToInt(P.Y / CellSize);
		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				int64 Key = (int64(cx + dx) << 32) | int64(uint32(cy + dy));
				TArray<int32> Cell;
				Grid.MultiFind(Key, Cell);
				for (int32 j : Cell)
				{
					if (FVector2D::DistSquared(P, Points[j].Pos) <= EpsSq)
						Nbrs.Add(j);
				}
			}
		}
		return Nbrs;
	};

	int32 ClusterId = 0;
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		if (Points[i].ClusterId >= 0) continue;

		TArray<int32> Neighbors = FindNeighbors(i);
		if (Neighbors.Num() < kDBSCAN_MinPts) continue;

		Points[i].ClusterId = ClusterId;
		TArray<int32> Queue = Neighbors;
		for (int32 qi = 0; qi < Queue.Num(); ++qi)
		{
			int32 idx = Queue[qi];
			if (Points[idx].ClusterId >= 0 && Points[idx].ClusterId != ClusterId) continue;
			Points[idx].ClusterId = ClusterId;

			TArray<int32> SubNeighbors = FindNeighbors(idx);
			if (SubNeighbors.Num() >= kDBSCAN_MinPts)
			{
				for (int32 sn : SubNeighbors)
				{
					if (Points[sn].ClusterId < 0) Queue.AddUnique(sn);
				}
			}
		}
		ClusterId++;
	}

	// 클러스터 → FPoiCluster 변환
	TMap<int32, FPoiCluster> ClusterMap;
	TMap<int32, TArray<FVector2D>> ClusterPoints;
	for (const FPt& P : Points)
	{
		if (P.ClusterId < 0) continue;
		FPoiCluster& C = ClusterMap.FindOrAdd(P.ClusterId);
		C.Centroid += P.Pos;
		C.BuildingCount++;
		C.AreaM2 += P.AreaM2;
		C.MaxHeightM = FMath::Max(C.MaxHeightM, P.HeightM);
		ClusterPoints.FindOrAdd(P.ClusterId).Add(P.Pos);
	}

	auto ComputeConvexHull = [](TArray<FVector2D>& Pts) -> TArray<FVector2D>
	{
		if (Pts.Num() < 3) return Pts;
		Pts.Sort([](const FVector2D& A, const FVector2D& B)
		{
			return (A.X < B.X) || (A.X == B.X && A.Y < B.Y);
		});

		TArray<FVector2D> Hull;
		for (const FVector2D& P : Pts)
		{
			while (Hull.Num() >= 2)
			{
				FVector2D A = Hull[Hull.Num() - 2];
				FVector2D B = Hull[Hull.Num() - 1];
				if ((B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X) <= 0.f)
					Hull.Pop();
				else break;
			}
			Hull.Add(P);
		}

		int32 Lower = Hull.Num() + 1;
		for (int32 i = Pts.Num() - 2; i >= 0; --i)
		{
			const FVector2D& P = Pts[i];
			while (Hull.Num() >= Lower)
			{
				FVector2D A = Hull[Hull.Num() - 2];
				FVector2D B = Hull[Hull.Num() - 1];
				if ((B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X) <= 0.f)
					Hull.Pop();
				else break;
			}
			Hull.Add(P);
		}
		Hull.Pop();
		return Hull;
	};

	for (auto& KV : ClusterMap)
	{
		FPoiCluster& C = KV.Value;
		if (C.BuildingCount > 0) C.Centroid /= C.BuildingCount;
		C.Score = C.BuildingCount * (C.AreaM2 / FMath::Max(1.f, (float)C.BuildingCount));

		if (TArray<FVector2D>* Pts = ClusterPoints.Find(KV.Key))
		{
			C.BoundaryPoints = ComputeConvexHull(*Pts);
		}

		Result.Add(MoveTemp(C));
	}

	// 스코어 순 정렬 → 등급 부여
	Result.Sort([](const FPoiCluster& A, const FPoiCluster& B)
	{
		return A.Score > B.Score;
	});

	for (int32 i = 0; i < Result.Num(); ++i)
	{
		const float Pct = (float)i / FMath::Max(1.f, (float)Result.Num());
		if      (Pct < 0.10f) Result[i].Grade = TEXT("S");
		else if (Pct < 0.30f) Result[i].Grade = TEXT("A");
		else if (Pct < 0.60f) Result[i].Grade = TEXT("B");
		else                   Result[i].Grade = TEXT("C");
		Result[i].Label = FString::Printf(TEXT("거점_%c_%d"),
			Result[i].Grade[0], i);
	}

	UE_LOG(LogChecklist, Log, TEXT("DBSCAN: %d 거점 탐지 (%d 건물)"),
		Result.Num(), Points.Num());
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BR 체크 구현
// ─────────────────────────────────────────────────────────────────────────────

FCheckResult UChecklistEngine::CheckBR1_PoiSpread(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-1");
	R.Label   = TEXT("거점 분산");

	TArray<const FPoiCluster*> SPois;
	for (const auto& P : Pois) if (P.Grade == TEXT("S")) SPois.Add(&P);

	if (SPois.Num() < 2) { R.Status = ECheckStatus::Pass; R.Score = 0; R.Detail = TEXT("S등급 거점 < 2"); return R; }

	const float MapSizeCm = Subsystem ? Subsystem->MapRadiusKm * 2.f * 100000.f : 200000.f;
	const float MinDist02 = MapSizeCm * 0.2f;
	const float MinDist01 = MapSizeCm * 0.1f;

	float WorstDist = FLT_MAX;
	for (int32 i = 0; i < SPois.Num(); ++i)
		for (int32 j = i + 1; j < SPois.Num(); ++j)
		{
			float D = FVector2D::Distance(SPois[i]->Centroid, SPois[j]->Centroid);
			WorstDist = FMath::Min(WorstDist, D);
		}

	if (WorstDist >= MinDist02)       { R.Status = ECheckStatus::Pass;    R.Score = 0; }
	else if (WorstDist >= MinDist01)  { R.Status = ECheckStatus::Warning; R.Score = -5; }
	else                               { R.Status = ECheckStatus::Fail;    R.Score = -15; }

	R.Detail = FString::Printf(TEXT("S등급 최소 간격 %.0fm (기준 %.0fm)"),
		WorstDist / 100.f, MinDist02 / 100.f);
	return R;
}

FCheckResult UChecklistEngine::CheckBR2_LootGradient(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-2");
	R.Label   = TEXT("루트 등급 그래디언트");

	int32 SOuterCount = 0;
	const float HalfRadius = Subsystem ? Subsystem->MapRadiusKm * 100000.f * 0.5f : 50000.f;

	for (const auto& P : Pois)
	{
		if (P.Grade == TEXT("S") && P.Centroid.Size() > HalfRadius)
			SOuterCount++;
	}

	if (SOuterCount == 0)      { R.Status = ECheckStatus::Pass;    R.Score = 0; }
	else if (SOuterCount == 1) { R.Status = ECheckStatus::Warning; R.Score = -5; }
	else                        { R.Status = ECheckStatus::Fail;    R.Score = -10; }

	R.Detail = FString::Printf(TEXT("외곽 S등급 %d개"), SOuterCount);
	return R;
}

FCheckResult UChecklistEngine::CheckBR3_ChokeBypass() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-3");
	R.Label   = TEXT("병목 우회로");

	FRoadGraphResult Graph = BuildRoadGraph();
	TArray<int32> Bridges = FindBridgeEdges(Graph);

	int32 BridgeType = 0, TunnelType = 0, NarrowType = 0, CrossingType = 0;

	for (int32 Idx : Bridges)
	{
		const FRoadEdge& E = Graph.Edges[Idx];

		bool bOverWater = false;
		if (Subsystem)
		{
			FVector2D NodeA2D = FVector2D::ZeroVector;
			FVector2D NodeB2D = FVector2D::ZeroVector;
			for (const auto& Road : Subsystem->EffectiveRoads)
			{
				for (int32 pi = 0; pi < Road.PointsUE5.Num(); ++pi)
				{
					int64 NodeHash = (int64)(Road.PointsUE5[pi].X * 0.01f) * 100000 + (int64)(Road.PointsUE5[pi].Y * 0.01f);
					if (NodeHash == E.NodeA) NodeA2D = Road.PointsUE5[pi];
					if (NodeHash == E.NodeB) NodeB2D = Road.PointsUE5[pi];
				}
			}
			FVector2D MidPt = (NodeA2D + NodeB2D) * 0.5f;

			for (const auto& W : Subsystem->CachedWaterAreas)
			{
				if (FVector2D::Distance(MidPt, W.Center) < W.RadiusCm)
				{
					bOverWater = true;
					break;
				}
			}
		}

		if (bOverWater)
		{
			++CrossingType;
		}
		else if (E.Weight < 3000.f) // very short edge = narrow pass
		{
			++NarrowType;
		}
		else if (E.Weight < 8000.f)
		{
			++TunnelType;
		}
		else
		{
			++BridgeType;
		}
	}

	int32 Total = Bridges.Num();
	if (Total == 0)      { R.Status = ECheckStatus::Pass;    R.Score = 0; }
	else if (Total == 1) { R.Status = ECheckStatus::Warning; R.Score = -5; }
	else                  { R.Status = ECheckStatus::Fail;    R.Score = -15; }

	R.Detail = FString::Printf(
		TEXT("병목 %d개 (교량:%d 터널:%d 협로:%d 수역횡단:%d)"),
		Total, BridgeType, TunnelType, NarrowType, CrossingType);

	if (Bridges.Num() > 0)
	{
		R.Suggestion = TEXT("병목 지점에 우회 도로를 추가하세요");
		const FRoadEdge& First = Graph.Edges[Bridges[0]];
		for (const auto& Road : Subsystem->EffectiveRoads)
		{
			if (Road.PointsUE5.Num() >= 2)
			{
				FVector2D Mid = (Road.PointsUE5[0] + Road.PointsUE5.Last()) * 0.5f;
				R.FocusLocation = FVector(Mid.X, Mid.Y, 0.f);
				break;
			}
		}
	}
	return R;
}

FCheckResult UChecklistEngine::CheckBR4_OpenClosedMix() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-4");
	R.Label   = TEXT("개방/폐쇄 혼합");

	float Coverage = ComputeBuildingCoveragePercent();

	if (Coverage >= 30.f && Coverage <= 50.f)      R.Status = ECheckStatus::Pass;
	else if (Coverage >= 20.f && Coverage <= 65.f)  R.Status = ECheckStatus::Warning;
	else                                             R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("건물 밀집 %.1f%% (기준 30~50%%)"), Coverage);
	return R;
}

FCheckResult UChecklistEngine::CheckBR5_CoverTheme() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-5");
	R.Label   = TEXT("엄폐물 테마 적합성");
	R.Status  = ECheckStatus::Pass;
	R.Score   = 0;
	R.Detail  = TEXT("지형+건물 기준 진단. 식생/바위 수동 배치 권장");
	return R;
}

FCheckResult UChecklistEngine::CheckBR6_VehicleRoute(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-6");
	R.Label   = TEXT("차량 이동 경로");

	const float DiagCm = Subsystem ? Subsystem->MapRadiusKm * 2.f * 100000.f * 1.414f : 200000.f;
	if (DiagCm < 200000.f) // < 2km
	{
		R.Status = ECheckStatus::Pass;
		R.Score  = 0;
		R.Detail = TEXT("소형 맵 — 차량 불필요");
		return R;
	}

	const int32 RoadCount = Subsystem ? Subsystem->EffectiveRoads.Num() : 0;
	R.Status = (RoadCount > 0) ? ECheckStatus::Pass : ECheckStatus::Fail;
	R.Score  = (RoadCount > 0) ? 0 : -10;
	R.Detail = FString::Printf(TEXT("도로 %d개"), RoadCount);
	return R;
}

FCheckResult UChecklistEngine::CheckBR7_ShrinkEquity() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-7");
	R.Label   = TEXT("수축 형평성");

	FRoadGraphResult Graph = BuildRoadGraph();
	int32 Blocked = CountBlockedDirections(Graph);

	if (Blocked == 0)      R.Status = ECheckStatus::Pass;
	else if (Blocked == 1) R.Status = ECheckStatus::Warning;
	else                    R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("%d/4 방향 접근 차단"), Blocked);
	if (Blocked > 0)
		R.Suggestion = TEXT("차단된 방향에 도로를 추가하세요");
	return R;
}

FCheckResult UChecklistEngine::CheckBR8_SizeDensity() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-8");
	R.Label   = TEXT("맵 크기별 밀도");

	if (!Subsystem || Subsystem->MapRadiusKm <= 0.f)
	{
		R.Status = ECheckStatus::NotApplicable;
		return R;
	}

	const float AreaKm2 = PI * Subsystem->MapRadiusKm * Subsystem->MapRadiusKm;
	const float DiamKm  = Subsystem->MapRadiusKm * 2.f;
	const float Density = static_cast<float>(Subsystem->EffectiveBuildings.Num()) / AreaKm2;

	float Target;
	if      (DiamKm <= 1.5f) Target = 80.f;
	else if (DiamKm <= 4.f)  Target = 50.f;
	else                      Target = 30.f;

	const float Pct = FMath::Abs(Density - Target) / Target * 100.f;

	if (Pct <= 20.f)      R.Status = ECheckStatus::Pass;
	else if (Pct <= 40.f)  R.Status = ECheckStatus::Warning;
	else                    R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("밀도 %.0f/km² (기준 %.0f ±20%%)"), Density, Target);
	return R;
}

FCheckResult UChecklistEngine::CheckBR9_VerticalCombat(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-9");
	R.Label   = TEXT("수직 전투 가능성");

	float MaxH = 0.f;
	for (const auto& P : Pois)
	{
		if (P.Grade == TEXT("S") || P.Grade == TEXT("A"))
			MaxH = FMath::Max(MaxH, P.MaxHeightM);
	}

	if (MaxH >= 10.f)       R.Status = ECheckStatus::Pass;
	else if (MaxH >= 5.f)   R.Status = ECheckStatus::Warning;
	else                     R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -2.5f : -10;
	R.Detail = FString::Printf(TEXT("고루트 거점 최대 높이 %.1fm"), MaxH);
	return R;
}

FCheckResult UChecklistEngine::CheckBR10_ExploreReward() const
{
	FCheckResult R;
	R.CheckId = TEXT("BR-10");
	R.Label   = TEXT("탐험 리워드");

	int32 Count = CountMarkersByType(TEXT("exploration_reward"))
	            + CountMarkersByType(TEXT("lock_marker"));

	if (Count >= 2)       R.Status = ECheckStatus::Pass;
	else if (Count == 1)  R.Status = ECheckStatus::Warning;
	else                   R.Status = ECheckStatus::NotApplicable;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -2.5f : 0;
	R.Detail = FString::Printf(TEXT("탐험/잠금 마커 %d개"), Count);
	if (Count == 0) R.Detail += TEXT(" (⬜ 미설정)");
	return R;
}

// ─────────────────────────────────────────────────────────────────────────────
//  EX 체크 구현
// ─────────────────────────────────────────────────────────────────────────────

FCheckResult UChecklistEngine::CheckEX1_ExtractRoutes() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-1");
	R.Label   = TEXT("탈출 루트 복수성");

	int32 ExtractCount = CountMarkersByType(TEXT("extraction_point"));
	if (ExtractCount == 0)
	{
		R.Status = ECheckStatus::NotApplicable;
		R.Detail = TEXT("추출 마커 미설정");
		return R;
	}

	if (ExtractCount >= 2)  R.Status = ECheckStatus::Pass;
	else                     R.Status = ECheckStatus::Warning;

	R.Score  = (R.Status == ECheckStatus::Pass) ? 0 : -10;
	R.Detail = FString::Printf(TEXT("추출점 %d개"), ExtractCount);
	return R;
}

FCheckResult UChecklistEngine::CheckEX2_RiskReward(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-2");
	R.Label   = TEXT("위험=보상 비례");

	if (!Subsystem) { R.Status = ECheckStatus::NotApplicable; return R; }

	TArray<const FPoiCluster*> SPois;
	for (const auto& P : Pois)
		if (P.Grade == TEXT("S")) SPois.Add(&P);

	bool bHasStealth = false, bHasCombat = false, bHasBypass = false;
	const FRoadGraphResult Graph = BuildRoadGraph();

	for (const FPoiCluster* S : SPois)
	{
		int32 LowCount = 0, MultiCount = 0;
		float TotalArea = 0.f;
		for (const auto& B : Subsystem->EffectiveBuildings)
		{
			if (FVector2D::Distance(B.CentroidUE5, S->Centroid) > 15000.f) continue;
			TotalArea += B.AreaM2;
			if (B.HeightM <= 5.f) LowCount++;
			if (B.HeightM > 8.f) MultiCount++;
		}
		float AvgSpacing = (S->BuildingCount > 1)
			? FMath::Sqrt(S->AreaM2 / FMath::Max(1, S->BuildingCount))
			: 9999.f;

		float LowRatio = (S->BuildingCount > 0)
			? static_cast<float>(LowCount) / S->BuildingCount : 0.f;

		if (AvgSpacing <= 20.f && LowRatio >= 0.5f) bHasStealth = true;

		float OpenPct = (S->AreaM2 > 0.f) ? (1.f - TotalArea / S->AreaM2) * 100.f : 100.f;
		if (MultiCount >= 3 && OpenPct >= 30.f) bHasCombat = true;

		int32 EntryEdges = 0;
		for (const auto& E : Graph.Edges)
		{
			auto PtOf = [&](int64 Key) -> FVector2D {
				return FVector2D(float(int32(Key >> 32)) * 1000.f, float(int32(uint32(Key))) * 1000.f);
			};
			FVector2D A = PtOf(E.NodeA), B2 = PtOf(E.NodeB);
			bool aIn = FVector2D::Distance(A, S->Centroid) < 15000.f;
			bool bIn = FVector2D::Distance(B2, S->Centroid) < 15000.f;
			if (aIn != bIn) EntryEdges++;
		}
		if (EntryEdges >= 2) bHasBypass = true;
	}

	int32 TypeCount = (bHasStealth ? 1 : 0) + (bHasCombat ? 1 : 0) + (bHasBypass ? 1 : 0);

	if (SPois.Num() >= 3 && TypeCount >= 2)      R.Status = ECheckStatus::Pass;
	else if (SPois.Num() >= 2)                     R.Status = ECheckStatus::Warning;
	else                                            R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -15;
	R.Detail = FString::Printf(TEXT("S등급 %d개, 유형 %d종 (잠입%d 교전%d 회피%d)"),
		SPois.Num(), TypeCount, bHasStealth, bHasCombat, bHasBypass);
	return R;
}

FCheckResult UChecklistEngine::CheckEX3_VerticalCombat(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-3");
	R.Label   = TEXT("수직 전투");

	float MaxH = 0.f;
	for (const auto& P : Pois)
	{
		if (P.Grade == TEXT("S") || P.Grade == TEXT("A"))
			MaxH = FMath::Max(MaxH, P.MaxHeightM);
	}

	if (MaxH >= 10.f)      R.Status = ECheckStatus::Pass;
	else if (MaxH > 0.f)   R.Status = ECheckStatus::Warning;
	else                    R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("최대 고저차 %.1fm (기준 10m+)"), MaxH);
	return R;
}

FCheckResult UChecklistEngine::CheckEX4_StealthRoute(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-4");
	R.Label   = TEXT("은신 루트");

	if (!Subsystem || Pois.Num() < 2)
	{
		R.Status = ECheckStatus::Fail;
		R.Score  = -10;
		R.Detail = TEXT("거점 2개 미만");
		return R;
	}

	const float ExposureLimitCm = Subsystem->MapRadiusKm * 200000.f;
	const float SampleStep = FMath::Max(1000.f, Subsystem->MapRadiusKm * 10000.f);
	float WorstExposureCm = 0.f;
	FString WorstPairLabel;

	for (int32 i = 0; i < Pois.Num(); ++i)
	{
		for (int32 j = i + 1; j < Pois.Num(); ++j)
		{
			const FVector2D& A = Pois[i].Centroid;
			const FVector2D& B = Pois[j].Centroid;
			float TotalDist = FVector2D::Distance(A, B);
			if (TotalDist < 1.f) continue;

			FVector2D Dir = (B - A) / TotalDist;
			float CurrentExposure = 0.f;
			float MaxExposure = 0.f;

			for (float T = 0.f; T <= TotalDist; T += SampleStep)
			{
				FVector2D Pt = A + Dir * T;

				bool bCovered = false;
				for (const auto& Bld : Subsystem->EffectiveBuildings)
				{
					if (FVector2D::Distance(Pt, Bld.CentroidUE5) < 5000.f)
					{
						bCovered = true;
						break;
					}
				}

				if (!bCovered)
					CurrentExposure += SampleStep;
				else
					CurrentExposure = 0.f;

				MaxExposure = FMath::Max(MaxExposure, CurrentExposure);
			}

			if (MaxExposure > WorstExposureCm)
			{
				WorstExposureCm = MaxExposure;
				WorstPairLabel = FString::Printf(TEXT("%s↔%s"), *Pois[i].Label, *Pois[j].Label);
			}
		}
	}

	const float WorstExposureM = WorstExposureCm / 100.f;

	if (WorstExposureCm <= ExposureLimitCm)        R.Status = ECheckStatus::Pass;
	else if (WorstExposureCm <= ExposureLimitCm * 1.5f) R.Status = ECheckStatus::Warning;
	else                                              R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("최대 노출 %.0fm (%s, 기준 ≤%.0fm)"),
		WorstExposureM, *WorstPairLabel, ExposureLimitCm / 100.f);
	return R;
}

FCheckResult UChecklistEngine::CheckEX5_ExtractTension() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-5");
	R.Label   = TEXT("추출 긴장감");

	if (!Subsystem) { R.Status = ECheckStatus::NotApplicable; return R; }

	UEditLayerManager* LM = Subsystem->GetLayerManager();
	if (!LM) { R.Status = ECheckStatus::NotApplicable; return R; }

	TArray<FVector2D> ExtractLocs;
	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (!L.bVisible || L.Type != EEditLayerType::Marker) continue;
		if (!L.Params.IsValid()) continue;
		if (L.Params->GetStringField(TEXT("marker_type")) != TEXT("extraction_point")) continue;
		ExtractLocs.Add(FVector2D(L.Area.Point.LocationUE5.X, L.Area.Point.LocationUE5.Y));
	}

	if (ExtractLocs.Num() == 0)
	{
		R.Status = ECheckStatus::NotApplicable;
		R.Detail = TEXT("추출 마커 미설정");
		return R;
	}

	const float CheckRadiusCm = 5000.f;
	int32 ExposedCount = 0;
	int32 ShieldedCount = 0;

	for (const FVector2D& Ext : ExtractLocs)
	{
		int32 NearbyBuildings = 0;
		for (const auto& B : Subsystem->EffectiveBuildings)
		{
			if (FVector2D::Distance(B.CentroidUE5, Ext) <= CheckRadiusCm)
				NearbyBuildings++;
		}

		if (NearbyBuildings <= 1)
			ExposedCount++;
		else
			ShieldedCount++;
	}

	if (ExposedCount >= 2)
		R.Status = ECheckStatus::Pass;
	else if (ExposedCount >= 1)
		R.Status = ECheckStatus::Warning;
	else
		R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -8;
	R.Detail = FString::Printf(TEXT("추출 %d개 중 노출 %d / 엄폐 %d (기준: 노출 2+)"),
		ExtractLocs.Num(), ExposedCount, ShieldedCount);
	return R;
}

FCheckResult UChecklistEngine::CheckEX6_RotationPaths(const TArray<FPoiCluster>& Pois) const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-6");
	R.Label   = TEXT("회전 경로 다양성");

	if (Pois.Num() < 2)
	{
		R.Status = ECheckStatus::Warning;
		R.Score  = -5;
		R.Detail = TEXT("거점 2개 미만 — 경로 분석 불가");
		return R;
	}

	FRoadGraphResult Graph = BuildRoadGraph();
	if (Graph.Edges.Num() == 0)
	{
		R.Status = ECheckStatus::Fail;
		R.Score  = -10;
		R.Detail = TEXT("도로 그래프 없음");
		return R;
	}

	const float SnapCm = 1000.f;
	auto NearestNode = [&](const FVector2D& Pt) -> int64 {
		int32 cx = FMath::RoundToInt(Pt.X / SnapCm);
		int32 cy = FMath::RoundToInt(Pt.Y / SnapCm);
		int64 Best = (int64(cx) << 32) | int64(uint32(cy));
		float BestDist = FLT_MAX;
		for (const auto& KV : Graph.AdjList)
		{
			FVector2D NodePt(float(int32(KV.Key >> 32)) * SnapCm,
			                 float(int32(uint32(KV.Key))) * SnapCm);
			float D = FVector2D::DistSquared(Pt, NodePt);
			if (D < BestDist) { BestDist = D; Best = KV.Key; }
		}
		return Best;
	};

	int32 PairsWithAlt = 0;
	int32 TotalPairs = 0;

	for (int32 i = 0; i < FMath::Min(Pois.Num(), 6); ++i)
	{
		for (int32 j = i + 1; j < FMath::Min(Pois.Num(), 6); ++j)
		{
			TotalPairs++;
			int64 NodeA = NearestNode(Pois[i].Centroid);
			int64 NodeB = NearestNode(Pois[j].Centroid);
			if (NodeA == NodeB) continue;

			// BFS 1: find shortest path
			TMap<int64, int64> ParentFirst;
			TArray<int64> Queue;
			Queue.Add(NodeA);
			ParentFirst.Add(NodeA, -1);
			bool bFound1 = false;
			while (Queue.Num() > 0 && !bFound1)
			{
				int64 Cur = Queue[0]; Queue.RemoveAt(0);
				const TArray<int32>* Adj = Graph.AdjList.Find(Cur);
				if (!Adj) continue;
				for (int32 EIdx : *Adj)
				{
					int64 Neigh = (Graph.Edges[EIdx].NodeA == Cur)
						? Graph.Edges[EIdx].NodeB : Graph.Edges[EIdx].NodeA;
					if (ParentFirst.Contains(Neigh)) continue;
					ParentFirst.Add(Neigh, Cur);
					Queue.Add(Neigh);
					if (Neigh == NodeB) { bFound1 = true; break; }
				}
			}
			if (!bFound1) continue;

			TSet<int64> Path1Nodes;
			{ int64 C = NodeB; while (C != -1) { Path1Nodes.Add(C); const int64* P = ParentFirst.Find(C); C = P ? *P : -1; } }

			// BFS 2: find alt path avoiding path1 interior nodes
			TMap<int64, int64> ParentSecond;
			TArray<int64> Queue2;
			Queue2.Add(NodeA);
			ParentSecond.Add(NodeA, -1);
			bool bFound2 = false;
			while (Queue2.Num() > 0 && !bFound2)
			{
				int64 Cur = Queue2[0]; Queue2.RemoveAt(0);
				const TArray<int32>* Adj = Graph.AdjList.Find(Cur);
				if (!Adj) continue;
				for (int32 EIdx : *Adj)
				{
					int64 Neigh = (Graph.Edges[EIdx].NodeA == Cur)
						? Graph.Edges[EIdx].NodeB : Graph.Edges[EIdx].NodeA;
					if (ParentSecond.Contains(Neigh)) continue;
					if (Neigh != NodeB && Path1Nodes.Contains(Neigh)) continue;
					ParentSecond.Add(Neigh, Cur);
					Queue2.Add(Neigh);
					if (Neigh == NodeB) { bFound2 = true; break; }
				}
			}
			if (bFound2) PairsWithAlt++;
		}
	}

	if (TotalPairs == 0)
	{
		R.Status = ECheckStatus::Warning;
		R.Score  = -5;
		R.Detail = TEXT("분석 가능한 거점 쌍 없음");
		return R;
	}

	float AltRatio = static_cast<float>(PairsWithAlt) / TotalPairs;

	if (AltRatio >= 0.8f)       R.Status = ECheckStatus::Pass;
	else if (AltRatio >= 0.5f)  R.Status = ECheckStatus::Warning;
	else                         R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("거점 쌍 %d/%d에 대안 경로 존재 (%.0f%%)"),
		PairsWithAlt, TotalPairs, AltRatio * 100.f);
	return R;
}

FCheckResult UChecklistEngine::CheckEX7_SoloSquadCover() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-7");
	R.Label   = TEXT("솔로/스쿼드 엄폐");

	float CoverPct = ComputeBuildingCoveragePercent();

	if (CoverPct >= 15.f)       R.Status = ECheckStatus::Pass;
	else if (CoverPct >= 10.f)  R.Status = ECheckStatus::Warning;
	else                         R.Status = ECheckStatus::Fail;

	R.Score = (R.Status == ECheckStatus::Pass) ? 0 :
	          (R.Status == ECheckStatus::Warning) ? -5 : -10;
	R.Detail = FString::Printf(TEXT("건물 엄폐 %.1f%% (기준 15%%)"), CoverPct);
	R.Suggestion = TEXT("식생/바위 수동 배치 권장");
	return R;
}

FCheckResult UChecklistEngine::CheckEX8_LockedLoot() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-8");
	R.Label   = TEXT("잠금 루트");

	int32 Count = CountMarkersByType(TEXT("lock_marker"));

	if (Count >= 2)       R.Status = ECheckStatus::Pass;
	else if (Count == 1)  R.Status = ECheckStatus::Warning;
	else                   R.Status = ECheckStatus::NotApplicable;

	R.Score  = (R.Status == ECheckStatus::Pass) ? 0 :
	           (R.Status == ECheckStatus::Warning) ? -2.5f : 0;
	R.Detail = FString::Printf(TEXT("잠금 마커 %d개"), Count);
	if (Count == 0) R.Detail += TEXT(" (⬜ 미설정)");
	return R;
}

FCheckResult UChecklistEngine::CheckEX9_NoiseTrigger() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-9");
	R.Label   = TEXT("소음 트리거");

	int32 Count = CountMarkersByType(TEXT("noise_trigger"));

	if (Count >= 3)       R.Status = ECheckStatus::Pass;
	else if (Count >= 1)  R.Status = ECheckStatus::Warning;
	else                   R.Status = ECheckStatus::NotApplicable;

	R.Score  = (R.Status == ECheckStatus::Pass) ? 0 :
	           (R.Status == ECheckStatus::Warning) ? -2.5f : 0;
	R.Detail = FString::Printf(TEXT("소음 마커 %d개"), Count);
	if (Count == 0) R.Detail += TEXT(" (⬜ 미설정)");
	return R;
}

FCheckResult UChecklistEngine::CheckEX10_AIZone() const
{
	FCheckResult R;
	R.CheckId = TEXT("EX-10");
	R.Label   = TEXT("AI/PvE 구역");

	int32 Count = CountMarkersByType(TEXT("ai_zone"));

	if (Count >= 2)       R.Status = ECheckStatus::Pass;
	else if (Count == 1)  R.Status = ECheckStatus::Warning;
	else                   R.Status = ECheckStatus::NotApplicable;

	R.Score  = (R.Status == ECheckStatus::Pass) ? 0 :
	           (R.Status == ECheckStatus::Warning) ? -2.5f : 0;
	R.Detail = FString::Printf(TEXT("AI 존 마커 %d개"), Count);
	if (Count == 0) R.Detail += TEXT(" (⬜ 미설정)");
	return R;
}

// ─────────────────────────────────────────────────────────────────────────────
//  헬퍼
// ─────────────────────────────────────────────────────────────────────────────

int32 UChecklistEngine::CountMarkersByType(const FString& MarkerType) const
{
	UEditLayerManager* LM = Subsystem ? Subsystem->GetLayerManager() : nullptr;
	if (!LM) return 0;

	int32 Count = 0;
	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (L.Type != EEditLayerType::Marker || !L.bVisible) continue;
		if (L.Params.IsValid() && L.Params->GetStringField(TEXT("marker_type")) == MarkerType)
			Count++;
	}
	return Count;
}

float UChecklistEngine::ComputeBuildingCoveragePercent() const
{
	if (!Subsystem || Subsystem->MapRadiusKm <= 0.f) return 0.f;

	float TotalBldgAreaM2 = 0.f;
	for (const auto& B : Subsystem->EffectiveBuildings)
		TotalBldgAreaM2 += B.AreaM2;

	const float MapAreaM2 = PI * FMath::Square(Subsystem->MapRadiusKm * 1000.f);
	return (MapAreaM2 > 0.f) ? (TotalBldgAreaM2 / MapAreaM2 * 100.f) : 0.f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  도로 그래프 구축 + Tarjan bridge edge 탐지
// ─────────────────────────────────────────────────────────────────────────────

UChecklistEngine::FRoadGraphResult UChecklistEngine::BuildRoadGraph() const
{
	if (!bRoadGraphDirty) return CachedRoadGraph;

	FRoadGraphResult G;
	if (!Subsystem) return G;

	const float SnapCm = 1000.f; // 10m
	auto CellKey = [SnapCm](const FVector2D& Pt) -> int64
	{
		int32 cx = FMath::FloorToInt(Pt.X / SnapCm);
		int32 cy = FMath::FloorToInt(Pt.Y / SnapCm);
		return (int64(cx) << 32) | int64(uint32(cy));
	};

	for (const auto& Road : Subsystem->EffectiveRoads)
	{
		if (Road.PointsUE5.Num() < 2) continue;
		int64 A = CellKey(Road.PointsUE5[0]);
		int64 B = CellKey(Road.PointsUE5.Last());
		if (A == B) continue;

		float Dist = FVector2D::Distance(Road.PointsUE5[0], Road.PointsUE5.Last());
		int32 Idx = G.Edges.Num();
		G.Edges.Add({ A, B, Dist, false });
		G.AdjList.FindOrAdd(A).Add(Idx);
		G.AdjList.FindOrAdd(B).Add(Idx);
	}

	CachedRoadGraph = G;
	bRoadGraphDirty = false;
	return CachedRoadGraph;
}

TArray<int32> UChecklistEngine::FindBridgeEdges(const FRoadGraphResult& Graph) const
{
	TArray<int32> Bridges;
	if (Graph.Edges.Num() == 0) return Bridges;

	TMap<int64, int32> Disc, Low;
	TSet<int64> Visited;
	int32 Timer = 0;

	TArray<int64> AllNodes;
	Graph.AdjList.GetKeys(AllNodes);

	struct FStackFrame { int64 Node; int64 Parent; int32 AdjIdx; };
	TArray<FStackFrame> Stack;

	for (int64 StartNode : AllNodes)
	{
		if (Visited.Contains(StartNode)) continue;

		Stack.Empty();
		Stack.Push({ StartNode, -1, 0 });
		Visited.Add(StartNode);
		Disc.Add(StartNode, Timer);
		Low.Add(StartNode, Timer);
		Timer++;

		while (Stack.Num() > 0)
		{
			FStackFrame& Frame = Stack.Last();
			const TArray<int32>* AdjEdges = Graph.AdjList.Find(Frame.Node);

			if (!AdjEdges || Frame.AdjIdx >= AdjEdges->Num())
			{
				int64 Child = Frame.Node;
				Stack.Pop();
				if (Stack.Num() > 0)
				{
					FStackFrame& Parent = Stack.Last();
					Low.FindOrAdd(Parent.Node) = FMath::Min(
						Low.FindChecked(Parent.Node), Low.FindChecked(Child));

					if (Low.FindChecked(Child) > Disc.FindChecked(Parent.Node))
					{
						// edge from Parent.Node to Child is a bridge
						// find which edge index connects them
						for (int32 EIdx : *Graph.AdjList.Find(Parent.Node))
						{
							const FRoadEdge& E = Graph.Edges[EIdx];
							int64 Other = (E.NodeA == Parent.Node) ? E.NodeB : E.NodeA;
							if (Other == Child) { Bridges.AddUnique(EIdx); break; }
						}
					}
				}
				continue;
			}

			int32 EdgeIdx = (*AdjEdges)[Frame.AdjIdx];
			Frame.AdjIdx++;

			const FRoadEdge& E = Graph.Edges[EdgeIdx];
			int64 Neighbor = (E.NodeA == Frame.Node) ? E.NodeB : E.NodeA;

			if (!Visited.Contains(Neighbor))
			{
				Visited.Add(Neighbor);
				Disc.Add(Neighbor, Timer);
				Low.Add(Neighbor, Timer);
				Timer++;
				Stack.Push({ Neighbor, Frame.Node, 0 });
			}
			else if (Neighbor != Frame.Parent)
			{
				Low.FindOrAdd(Frame.Node) = FMath::Min(
					Low.FindChecked(Frame.Node), Disc.FindChecked(Neighbor));
			}
		}
	}

	return Bridges;
}

int32 UChecklistEngine::CountBlockedDirections(const FRoadGraphResult& Graph) const
{
	if (!Subsystem || Subsystem->MapRadiusKm <= 0.f) return 0;

	const float RadCm = Subsystem->MapRadiusKm * 100000.f;
	const float ThreshCm = RadCm * 0.5f;

	// 4 directions: N(+Y), S(-Y), E(+X), W(-X)
	const FVector2D Dirs[4] = {
		FVector2D(0, 1), FVector2D(0, -1), FVector2D(1, 0), FVector2D(-1, 0)
	};

	TSet<int64> AllNodes;
	Graph.AdjList.GetKeys(AllNodes);

	const float SnapCm = 1000.f;
	auto NodePos = [SnapCm](int64 Key) -> FVector2D
	{
		int32 cx = int32(Key >> 32);
		int32 cy = int32(uint32(Key));
		return FVector2D(cx * SnapCm + SnapCm * 0.5f, cy * SnapCm + SnapCm * 0.5f);
	};

	int32 Blocked = 0;
	for (int32 d = 0; d < 4; ++d)
	{
		bool bReached = false;
		for (int64 Node : AllNodes)
		{
			FVector2D Pos = NodePos(Node);
			float Proj = FVector2D::DotProduct(Pos, Dirs[d]);
			if (Proj > ThreshCm) { bReached = true; break; }
		}
		if (!bReached) Blocked++;
	}
	return Blocked;
}

// ─────────────────────────────────────────────────────────────────────────────
//  가중 적합도 (대시보드)
// ─────────────────────────────────────────────────────────────────────────────

float UChecklistEngine::GetBRWeight(const FString& CheckId)
{
	if (CheckId == TEXT("BR-1"))  return 15.f;
	if (CheckId == TEXT("BR-2"))  return 15.f;
	if (CheckId == TEXT("BR-3"))  return 15.f;
	if (CheckId == TEXT("BR-4"))  return 12.f;
	if (CheckId == TEXT("BR-5"))  return 8.f;
	if (CheckId == TEXT("BR-6"))  return 8.f;
	if (CheckId == TEXT("BR-7"))  return 10.f;
	if (CheckId == TEXT("BR-8"))  return 7.f;
	if (CheckId == TEXT("BR-9"))  return 5.f;
	if (CheckId == TEXT("BR-10")) return 5.f;
	return 0.f;
}

float UChecklistEngine::GetEXWeight(const FString& CheckId)
{
	if (CheckId == TEXT("EX-1"))  return 15.f;
	if (CheckId == TEXT("EX-2"))  return 15.f;
	if (CheckId == TEXT("EX-3"))  return 10.f;
	if (CheckId == TEXT("EX-4"))  return 12.f;
	if (CheckId == TEXT("EX-5"))  return 8.f;
	if (CheckId == TEXT("EX-6"))  return 10.f;
	if (CheckId == TEXT("EX-7"))  return 8.f;
	if (CheckId == TEXT("EX-8"))  return 7.f;
	if (CheckId == TEXT("EX-9"))  return 8.f;
	if (CheckId == TEXT("EX-10")) return 7.f;
	return 0.f;
}

float UChecklistEngine::ComputeWeightedFitness(const FCheckReport& Report) const
{
	float Total = 0.f;

	for (const FCheckResult& R : Report.Results)
	{
		float W = (Report.Ruleset == ERulesetType::BR)
			? GetBRWeight(R.CheckId) : GetEXWeight(R.CheckId);

		float Mult = 0.f;
		switch (R.Status)
		{
		case ECheckStatus::Pass:          Mult = 1.f;  break;
		case ECheckStatus::Warning:       Mult = 0.5f; break;
		case ECheckStatus::Fail:          Mult = 0.f;  break;
		case ECheckStatus::NotApplicable: Mult = 1.f;  break;
		}
		Total += W * Mult;
	}

	return FMath::Clamp(Total, 0.f, 100.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  레퍼런스 유사도
// ─────────────────────────────────────────────────────────────────────────────

TArray<UChecklistEngine::FReferenceSimilarity> UChecklistEngine::ComputeReferenceSimilarity() const
{
	TArray<FReferenceSimilarity> Result;
	if (!Subsystem) return Result;

	float Current[5];
	Current[0] = Subsystem->GetSliderState(ESliderType::UrbanDensity).CurrentValue;
	Current[1] = Subsystem->GetSliderState(ESliderType::Openness).CurrentValue;
	Current[2] = Subsystem->GetSliderState(ESliderType::RouteComplexity).CurrentValue;
	Current[3] = Subsystem->GetSliderState(ESliderType::ElevationContrast).CurrentValue;
	Current[4] = Subsystem->GetSliderState(ESliderType::DestructionLevel).CurrentValue;

	for (const FMapPreset& P : LevelToolPresets::GetBuiltInPresets())
	{
		float Preset[5] = {
			P.Sliders.UrbanDensity, P.Sliders.Openness,
			P.Sliders.RouteComplexity, P.Sliders.ElevationContrast,
			P.Sliders.DestructionLevel
		};

		float SumSq = 0.f;
		for (int32 i = 0; i < 5; ++i)
		{
			float D = (Current[i] - Preset[i]) / 100.f;
			SumSq += D * D;
		}
		float Distance = FMath::Sqrt(SumSq);
		float Similarity = FMath::Max(0.f, (1.f - Distance / FMath::Sqrt(5.f)) * 100.f);

		Result.Add({ P.PresetName, Similarity });
	}

	Result.Sort([](const FReferenceSimilarity& A, const FReferenceSimilarity& B)
	{
		return A.SimilarityPct > B.SimilarityPct;
	});

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  제안 카드 생성
// ─────────────────────────────────────────────────────────────────────────────

void UChecklistEngine::GenerateSuggestions(const FCheckReport& Report, const TArray<FPoiCluster>& Pois)
{
	Suggestions.Empty();
	SuggestionCounter = 0;

	for (const FCheckResult& R : Report.Results)
	{
		if (R.Status != ECheckStatus::Fail && R.Status != ECheckStatus::Warning
			&& R.Status != ECheckStatus::NotApplicable) continue;

		// BR-10, EX-8~10: 마커 미설정(NotApplicable) → 마커 제안
		if (R.CheckId == TEXT("BR-10") || R.CheckId == TEXT("EX-8") ||
		    R.CheckId == TEXT("EX-9") || R.CheckId == TEXT("EX-10"))
		{
			FString MarkerType;
			if (R.CheckId == TEXT("BR-10")) MarkerType = TEXT("exploration_reward");
			else if (R.CheckId == TEXT("EX-8")) MarkerType = TEXT("lock_marker");
			else if (R.CheckId == TEXT("EX-9")) MarkerType = TEXT("noise_trigger");
			else MarkerType = TEXT("ai_zone");

			FVector2D Loc = FVector2D::ZeroVector;
			if (Pois.Num() > 0)
			{
				for (const auto& P : Pois)
				{
					if (P.Grade == TEXT("S") || P.Grade == TEXT("A"))
					{
						Loc = P.Centroid;
						break;
					}
				}
				if (Loc.IsZero()) Loc = Pois[0].Centroid;
			}

			Suggestions.Add(MakeMarkerSuggestion(R.CheckId, R.Detail, MarkerType, Loc,
				R.CheckId == TEXT("BR-10")
					? TEXT("Erangel 비밀의 방, Verdansk 벙커 레퍼런스")
					: TEXT("Tarkov 마크방, Hunt 소음 트랩 레퍼런스")));
		}

		// BR-3: 병목에 도로 추가
		if (R.CheckId == TEXT("BR-3"))
		{
			FRoadGraphResult Graph = BuildRoadGraph();
			TArray<int32> Bridges = FindBridgeEdges(Graph);
			const float SnapCm = 1000.f;
			for (int32 Bi : Bridges)
			{
				if (Bi >= Graph.Edges.Num()) continue;
				const FRoadEdge& E = Graph.Edges[Bi];
				FVector2D Start(float(int32(E.NodeA >> 32)) * SnapCm, float(int32(uint32(E.NodeA))) * SnapCm);
				FVector2D End(float(int32(E.NodeB >> 32)) * SnapCm, float(int32(uint32(E.NodeB))) * SnapCm);

				FVector2D Mid = (Start + End) * 0.5f;
				FVector2D Perp = FVector2D(-(End.Y - Start.Y), End.X - Start.X);
				Perp.Normalize();
				FVector2D BypassStart = Mid + Perp * 15000.f;
				FVector2D BypassEnd   = Mid - Perp * 15000.f;

				Suggestions.Add(MakeRoadAddSuggestion(R.CheckId,
					TEXT("병목 지점 우회 도로 필요"),
					BypassStart, BypassEnd,
					TEXT("Erangel: 동서 다리 + 캣워크로 우회 보장")));

				if (Suggestions.Num() >= 3) break;
			}
		}

		// BR-4: 밀도 과다 → 건물 제거
		if (R.CheckId == TEXT("BR-4") && R.Status == ECheckStatus::Fail)
		{
			FVector2D Loc = FVector2D::ZeroVector;
			float MinArea = FLT_MAX;
			for (const auto& B : Subsystem->EffectiveBuildings)
			{
				if (B.AreaM2 < MinArea) { MinArea = B.AreaM2; Loc = B.CentroidUE5; }
			}
			Suggestions.Add(MakeBuildingRemoveSuggestion(R.CheckId,
				TEXT("건물 밀집 과다 — 소형 건물 제거 권장"), Loc,
				TEXT("Erangel: 30% 밀집 기준")));
		}

		// EX-1: 추출점 배치
		if (R.CheckId == TEXT("EX-1") && R.Status == ECheckStatus::NotApplicable)
		{
			FVector2D EdgeLoc = FVector2D::ZeroVector;
			if (Subsystem && Subsystem->EffectiveRoads.Num() > 0)
			{
				float MaxDist = 0.f;
				for (const auto& Road : Subsystem->EffectiveRoads)
				{
					if (Road.PointsUE5.Num() == 0) continue;
					float D = Road.PointsUE5.Last().Size();
					if (D > MaxDist) { MaxDist = D; EdgeLoc = Road.PointsUE5.Last(); }
				}
			}
			Suggestions.Add(MakeMarkerSuggestion(R.CheckId,
				TEXT("추출점이 설정되지 않았습니다"),
				TEXT("extraction_point"), EdgeLoc,
				TEXT("맵 경계 도로 끝점에 추출점 배치 권장")));
		}
	}

	UE_LOG(LogChecklist, Log, TEXT("제안 카드 %d건 생성"), Suggestions.Num());
}

FSuggestionCard UChecklistEngine::MakeMarkerSuggestion(const FString& CheckId, const FString& Problem,
	const FString& MarkerType, const FVector2D& Location, const FString& Reference) const
{
	FSuggestionCard Card;
	Card.CardId   = FString::Printf(TEXT("suggest_%s_%d"), *CheckId, SuggestionCounter);
	Card.CheckId  = CheckId;
	Card.Problem  = Problem;
	Card.Reference = Reference;
	Card.FocusLocation = FVector(Location.X, Location.Y, 0.f);

	FEditLayer Layer;
	Layer.LayerId   = FGuid::NewGuid().ToString();
	Layer.Type      = EEditLayerType::Marker;
	Layer.Label     = FString::Printf(TEXT("[제안] %s 마커"), *MarkerType);
	Layer.CreatedBy = ELayerCreatedBy::AiSuggest;

	Layer.Area.Type = EAreaType::Point;
	Layer.Area.Point.LocationUE5 = FVector(Location.X, Location.Y, 0.f);

	Layer.Params = MakeShared<FJsonObject>();
	Layer.Params->SetStringField(TEXT("marker_type"), MarkerType);

	Card.SuggestedLayer = MoveTemp(Layer);
	++SuggestionCounter;
	return Card;
}

FSuggestionCard UChecklistEngine::MakeBuildingRemoveSuggestion(const FString& CheckId, const FString& Problem,
	const FVector2D& Location, const FString& Reference) const
{
	FSuggestionCard Card;
	Card.CardId   = FString::Printf(TEXT("suggest_%s_%d"), *CheckId, SuggestionCounter);
	Card.CheckId  = CheckId;
	Card.Problem  = Problem;
	Card.Reference = Reference;
	Card.FocusLocation = FVector(Location.X, Location.Y, 0.f);

	// Find the nearest building's StableId for precise removal
	FString NearestStableId;
	if (Subsystem)
	{
		float BestDist = FLT_MAX;
		for (const auto& B : Subsystem->EffectiveBuildings)
		{
			float D = FVector2D::Distance(B.CentroidUE5, Location);
			if (D < BestDist)
			{
				BestDist = D;
				NearestStableId = FString::Printf(TEXT("bldg_osm_%lld"), B.OsmId);
			}
		}
	}

	FEditLayer Layer;
	Layer.LayerId   = FGuid::NewGuid().ToString();
	Layer.Type      = EEditLayerType::BuildingRemove;
	Layer.Label     = FString::Printf(TEXT("[제안] 건물 제거 (%s)"), *CheckId);
	Layer.CreatedBy = ELayerCreatedBy::AiSuggest;

	Layer.Area.Type = EAreaType::ActorRef;
	Layer.Area.ActorRef.StableId = NearestStableId;
	Layer.Area.ActorRef.FallbackLocationUE5 = FVector(Location.X, Location.Y, 0.f);

	Layer.Params = MakeShared<FJsonObject>();
	Layer.Params->SetBoolField(TEXT("keep_foundation"), false);

	Card.SuggestedLayer = MoveTemp(Layer);
	++SuggestionCounter;
	return Card;
}

TArray<FVector2D> UChecklistEngine::AStarPath(const FVector2D& Start, const FVector2D& End) const
{
	const float CellSize = 2000.f; // 20m grid

	auto ToGrid = [&](const FVector2D& V) -> FIntPoint {
		return FIntPoint(FMath::RoundToInt(V.X / CellSize), FMath::RoundToInt(V.Y / CellSize));
	};
	auto FromGrid = [&](FIntPoint P) -> FVector2D {
		return FVector2D(P.X * CellSize, P.Y * CellSize);
	};

	TSet<FIntPoint> Blocked;
	TSet<FIntPoint> WaterCells;
	if (Subsystem)
	{
		for (const auto& B : Subsystem->EffectiveBuildings)
		{
			float HalfSize = FMath::Sqrt(B.AreaM2) * 50.f;
			FIntPoint Min = ToGrid(B.CentroidUE5 - FVector2D(HalfSize));
			FIntPoint Max = ToGrid(B.CentroidUE5 + FVector2D(HalfSize));
			for (int32 gx = Min.X; gx <= Max.X; ++gx)
				for (int32 gy = Min.Y; gy <= Max.Y; ++gy)
					Blocked.Add(FIntPoint(gx, gy));
		}
		for (const auto& W : Subsystem->CachedWaterAreas)
		{
			FIntPoint WG = ToGrid(W.Center);
			int32 R = FMath::CeilToInt(W.RadiusCm / CellSize);
			for (int32 gx = WG.X - R; gx <= WG.X + R; ++gx)
				for (int32 gy = WG.Y - R; gy <= WG.Y + R; ++gy)
					WaterCells.Add(FIntPoint(gx, gy));
		}
	}

	FIntPoint StartG = ToGrid(Start);
	FIntPoint EndG   = ToGrid(End);

	struct FNode
	{
		FIntPoint Pos;
		float G = FLT_MAX, F = FLT_MAX;
		FIntPoint Parent = FIntPoint(-999, -999);
	};

	TMap<FIntPoint, FNode> Nodes;
	TArray<FIntPoint> Open;

	auto Heuristic = [](FIntPoint A, FIntPoint B) -> float {
		return FMath::Abs(A.X - B.X) + FMath::Abs(A.Y - B.Y);
	};

	FNode& StartNode = Nodes.FindOrAdd(StartG);
	StartNode.Pos = StartG;
	StartNode.G = 0.f;
	StartNode.F = Heuristic(StartG, EndG);
	Open.Add(StartG);

	static const FIntPoint Dirs[] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1} };
	const int32 MaxIter = 2000;

	for (int32 Iter = 0; Iter < MaxIter && Open.Num() > 0; ++Iter)
	{
		int32 BestIdx = 0;
		for (int32 i = 1; i < Open.Num(); ++i)
		{
			if (Nodes[Open[i]].F < Nodes[Open[BestIdx]].F) BestIdx = i;
		}
		FIntPoint Current = Open[BestIdx];
		Open.RemoveAtSwap(BestIdx);

		if (Current == EndG)
		{
			TArray<FVector2D> Path;
			FIntPoint C = EndG;
			while (C != FIntPoint(-999, -999))
			{
				Path.Insert(FromGrid(C), 0);
				C = Nodes[C].Parent;
			}

			// Douglas-Peucker simplification (epsilon = CellSize)
			if (Path.Num() > 3)
			{
				TArray<FVector2D> Simplified;
				Simplified.Add(Path[0]);
				for (int32 i = 1; i < Path.Num() - 1; ++i)
				{
					FVector2D Prev = Simplified.Last();
					FVector2D Next = Path[FMath::Min(i + 1, Path.Num() - 1)];
					FVector2D Dir2 = (Next - Prev).GetSafeNormal();
					FVector2D ToP  = Path[i] - Prev;
					float Cross = FMath::Abs(Dir2.X * ToP.Y - Dir2.Y * ToP.X);
					if (Cross > CellSize)
						Simplified.Add(Path[i]);
				}
				Simplified.Add(Path.Last());
				return Simplified;
			}
			return Path;
		}

		for (const FIntPoint& D : Dirs)
		{
			FIntPoint Next(Current.X + D.X, Current.Y + D.Y);
			if (Blocked.Contains(Next)) continue;

			float MoveCost = (D.X != 0 && D.Y != 0) ? 1.414f : 1.f;
			if (WaterCells.Contains(Next)) MoveCost *= 5.f;
			float NewG = Nodes[Current].G + MoveCost;

			FNode& NNode = Nodes.FindOrAdd(Next);
			if (NNode.G == FLT_MAX) NNode.Pos = Next;
			if (NewG < NNode.G)
			{
				NNode.G = NewG;
				NNode.F = NewG + Heuristic(Next, EndG);
				NNode.Parent = Current;
				Open.AddUnique(Next);
			}
		}
	}

	return { Start, (Start + End) * 0.5f, End };
}

FSuggestionCard UChecklistEngine::MakeRoadAddSuggestion(const FString& CheckId, const FString& Problem,
	const FVector2D& Start, const FVector2D& End, const FString& Reference) const
{
	FSuggestionCard Card;
	Card.CardId   = FString::Printf(TEXT("suggest_%s_%d"), *CheckId, SuggestionCounter);
	Card.CheckId  = CheckId;
	Card.Problem  = Problem;
	Card.Reference = Reference;
	Card.FocusLocation = FVector((Start.X + End.X) * 0.5f, (Start.Y + End.Y) * 0.5f, 0.f);

	FEditLayer Layer;
	Layer.LayerId   = FGuid::NewGuid().ToString();
	Layer.Type      = EEditLayerType::RoadAdd;
	Layer.Label     = FString::Printf(TEXT("[제안] 우회 도로 (%s)"), *CheckId);
	Layer.CreatedBy = ELayerCreatedBy::AiSuggest;

	Layer.Area.Type = EAreaType::Path;
	Layer.Area.Path.PointsUE5 = AStarPath(Start, End);
	Layer.Area.Path.WidthCm   = 600.f;

	Layer.Params = MakeShared<FJsonObject>();
	Layer.Params->SetStringField(TEXT("road_type"), TEXT("minor"));
	Layer.Params->SetNumberField(TEXT("width_m"), 4.0);

	Card.SuggestedLayer = MoveTemp(Layer);
	++SuggestionCounter;
	return Card;
}

// ─────────────────────────────────────────────────────────────────────────────
//  제안 수락/거절
// ─────────────────────────────────────────────────────────────────────────────

bool UChecklistEngine::AcceptSuggestion(const FString& CardId)
{
	for (FSuggestionCard& Card : Suggestions)
	{
		if (Card.CardId != CardId) continue;
		if (Card.Status != ESuggestionStatus::Pending) return false;

		UEditLayerManager* LM = Subsystem ? Subsystem->GetLayerManager() : nullptr;
		if (!LM) return false;

		FEditLayer LayerCopy = Card.SuggestedLayer;
		const FString LayerId = LayerCopy.LayerId;
		LM->AddLayer(MoveTemp(LayerCopy));

		if (UEditLayerApplicator* App = LM->GetApplicator())
			App->ApplyLayer(LayerId);

		Card.CreatedLayerId = Card.SuggestedLayer.LayerId;
		Card.Status = ESuggestionStatus::Accepted;
		LM->SaveToJson();

		UE_LOG(LogChecklist, Log, TEXT("제안 수락: %s → 레이어 %s"), *CardId, *Card.CreatedLayerId);
		return true;
	}
	return false;
}

bool UChecklistEngine::RejectSuggestion(const FString& CardId)
{
	for (FSuggestionCard& Card : Suggestions)
	{
		if (Card.CardId != CardId) continue;
		if (Card.Status != ESuggestionStatus::Pending) return false;
		Card.Status = ESuggestionStatus::Rejected;
		UE_LOG(LogChecklist, Log, TEXT("제안 거절: %s"), *CardId);
		return true;
	}
	return false;
}

int32 UChecklistEngine::AcceptAllSuggestions()
{
	UEditLayerManager* LM = Subsystem ? Subsystem->GetLayerManager() : nullptr;
	if (!LM) return 0;

	FScopedTransaction Tx(FText::FromString(TEXT("모든 제안 일괄 수락")));

	int32 Count = 0;
	for (FSuggestionCard& Card : Suggestions)
	{
		if (Card.Status != ESuggestionStatus::Pending) continue;

		FEditLayer LayerCopy = Card.SuggestedLayer;
		const FString LayerId = LayerCopy.LayerId;
		LM->AddLayer(MoveTemp(LayerCopy));

		if (UEditLayerApplicator* App = LM->GetApplicator())
			App->ApplyLayer(LayerId);

		Card.CreatedLayerId = Card.SuggestedLayer.LayerId;
		Card.Status = ESuggestionStatus::Accepted;
		++Count;
	}

	if (Count > 0) LM->SaveToJson();
	UE_LOG(LogChecklist, Log, TEXT("제안 일괄 수락: %d건"), Count);
	return Count;
}

int32 UChecklistEngine::RejectAllSuggestions()
{
	int32 Count = 0;
	for (FSuggestionCard& Card : Suggestions)
	{
		if (Card.Status != ESuggestionStatus::Pending) continue;
		Card.Status = ESuggestionStatus::Rejected;
		++Count;
	}
	UE_LOG(LogChecklist, Log, TEXT("제안 일괄 거절: %d건"), Count);
	return Count;
}
