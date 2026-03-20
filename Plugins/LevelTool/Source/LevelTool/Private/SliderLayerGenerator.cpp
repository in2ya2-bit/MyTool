#include "SliderLayerGenerator.h"
#include "DesignerIntentSubsystem.h"
#include "EditLayerManager.h"
#include "EditLayerTypes.h"

#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogSliderGen, Log, All);

static const FVector2D kDirections8[] =
{
	FVector2D( 1.0f,  0.0f),
	FVector2D( 0.7071f,  0.7071f),
	FVector2D( 0.0f,  1.0f),
	FVector2D(-0.7071f,  0.7071f),
	FVector2D(-1.0f,  0.0f),
	FVector2D(-0.7071f, -0.7071f),
	FVector2D( 0.0f, -1.0f),
	FVector2D( 0.7071f, -0.7071f),
};
static const float kCosConeHalf = FMath::Cos(FMath::DegreesToRadians(22.5f));

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization
// ─────────────────────────────────────────────────────────────────────────────

void USliderLayerGenerator::Initialize(UDesignerIntentSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public dispatch
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateLayers(
	ESliderType Type, float CurrentValue, float NewValue) const
{
	if (FMath::IsNearlyEqual(CurrentValue, NewValue, 0.5f))
		return {};

	switch (Type)
	{
	case ESliderType::UrbanDensity:      return GenerateUrbanDensity(CurrentValue, NewValue);
	case ESliderType::Openness:          return GenerateOpenness(CurrentValue, NewValue);
	case ESliderType::RouteComplexity:   return GenerateRouteComplexity(CurrentValue, NewValue);
	case ESliderType::ElevationContrast: return GenerateElevationContrast(CurrentValue, NewValue);
	case ESliderType::DestructionLevel:  return GenerateDestructionLevel(CurrentValue, NewValue);
	default:
		return {};
	}
}

FString USliderLayerGenerator::MakeSourceId(ESliderType Type)
{
	switch (Type)
	{
	case ESliderType::UrbanDensity:     return TEXT("slider_urban_density");
	case ESliderType::Openness:         return TEXT("slider_openness");
	case ESliderType::RouteComplexity:  return TEXT("slider_route_complexity");
	case ESliderType::ElevationContrast:return TEXT("slider_elevation_contrast");
	case ESliderType::DestructionLevel: return TEXT("slider_destruction_level");
	default:                            return TEXT("slider_unknown");
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  경량 시뮬레이션 — 20% 샘플링으로 예상 레이어 수 산출
// ─────────────────────────────────────────────────────────────────────────────

USliderLayerGenerator::FLightweightEstimate USliderLayerGenerator::EstimateLightweight(
	ESliderType Type, float CurrentValue, float NewValue) const
{
	FLightweightEstimate Est;
	if (!Subsystem) return Est;

	const float Delta = NewValue - CurrentValue;
	if (FMath::IsNearlyZero(Delta)) return Est;

	const float Ratio = FMath::Abs(Delta) / FMath::Max(100.f - FMath::Min(CurrentValue, NewValue), 1.f);

	switch (Type)
	{
	case ESliderType::UrbanDensity:
	{
		if (Delta > 0)
		{
			TArray<FVector2D> EmptyCells = FindEmptyCells();
			int32 SampledCells = FMath::Max(1, EmptyCells.Num() / 5);
			Est.AddCount = FMath::CeilToInt(SampledCells * Ratio);
			Est.EstimatedAchievementPct = (EmptyCells.Num() > 0)
				? FMath::Min(100.f, (float)Est.AddCount / FMath::Max(1, FMath::CeilToInt(EmptyCells.Num() * Ratio)) * 100.f)
				: 0.f;
		}
		else
		{
			int32 TotalBuildings = Subsystem->CachedBuildings.Num();
			int32 SampledBuildings = FMath::Max(1, TotalBuildings / 5);
			Est.RemoveCount = FMath::CeilToInt(SampledBuildings * Ratio);
			Est.EstimatedAchievementPct = 100.f;
		}
		break;
	}
	case ESliderType::Openness:
	{
		TArray<FBuildingInfo> Bldgs = GetBuildingInfos();
		int32 Sampled = FMath::Max(1, Bldgs.Num() / 5);
		if (Delta > 0)
		{
			Est.RemoveCount = FMath::CeilToInt(Sampled * Ratio);
		}
		else
		{
			TArray<FVector2D> EmptyCells = FindEmptyCells();
			Est.AddCount = FMath::CeilToInt(EmptyCells.Num() / 5 * Ratio);
		}
		Est.EstimatedAchievementPct = 95.f;
		break;
	}
	case ESliderType::RouteComplexity:
	{
		int32 RoadCount = Subsystem->CachedRoads.Num();
		if (Delta > 0)
			Est.AddCount = FMath::CeilToInt(RoadCount * Ratio * 0.5f);
		else
			Est.RemoveCount = FMath::CeilToInt(RoadCount * Ratio * 0.3f);
		Est.EstimatedAchievementPct = 90.f;
		break;
	}
	case ESliderType::ElevationContrast:
	case ESliderType::DestructionLevel:
	{
		int32 Count = (Type == ESliderType::DestructionLevel)
			? Subsystem->CachedBuildings.Num()
			: 4;
		Est.AddCount = FMath::CeilToInt(Count * Ratio);
		Est.EstimatedAchievementPct = 95.f;
		break;
	}
	}

	return Est;
}

// ─────────────────────────────────────────────────────────────────────────────
//  UrbanDensity
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateUrbanDensity(
	float Current, float Target) const
{
	if (Target > Current)
	{
		const float Ratio = (Target - Current) / FMath::Max(100.f - Current, 1.f);
		return GenerateDensityIncrease(Ratio);
	}
	else
	{
		const float Ratio = (Current - Target) / FMath::Max(Current, 1.f);
		return GenerateDensityDecrease(Ratio);
	}
}

TArray<FEditLayer> USliderLayerGenerator::GenerateDensityIncrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::UrbanDensity);

	TArray<FVector2D> EmptyCells = FindEmptyCells();
	if (EmptyCells.Num() == 0)
	{
		UE_LOG(LogSliderGen, Warning, TEXT("밀도 증가: 빈 셀 없음"));
		return Result;
	}

	const int32 Count = FMath::CeilToInt(EmptyCells.Num() * Ratio);
	const int32 ActualCount = FMath::Min(Count, EmptyCells.Num());

	// 맵 중심 가까운 순으로 정렬 — 도심부터 채우기
	EmptyCells.Sort([](const FVector2D& A, const FVector2D& B)
	{
		return A.SizeSquared() < B.SizeSquared();
	});

	const float AvgHeightM = 12.f;
	for (int32 i = 0; i < ActualCount; ++i)
	{
		const float HeightM = AvgHeightM + FMath::FRandRange(-4.f, 8.f);
		Result.Add(MakeBuildingAddLayer(EmptyCells[i], HeightM, SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("밀도 증가: %d 건물 추가 레이어 생성 (비율 %.1f%%)"),
		ActualCount, Ratio * 100.f);
	return Result;
}

TArray<FEditLayer> USliderLayerGenerator::GenerateDensityDecrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::UrbanDensity);

	TArray<FBuildingInfo> Buildings = GetBuildingInfos();
	if (Buildings.Num() == 0) return Result;

	// 맵 중심에서 먼 거리 순 정렬 — 외곽부터 제거
	Buildings.Sort([](const FBuildingInfo& A, const FBuildingInfo& B)
	{
		return A.CentroidUE5.SizeSquared() > B.CentroidUE5.SizeSquared();
	});

	const int32 Count = FMath::CeilToInt(Buildings.Num() * Ratio);
	const int32 ActualCount = FMath::Min(Count, Buildings.Num());

	for (int32 i = 0; i < ActualCount; ++i)
	{
		Result.Add(MakeBuildingRemoveLayer(Buildings[i], SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("밀도 감소: %d 건물 제거 레이어 생성 (비율 %.1f%%)"),
		ActualCount, Ratio * 100.f);
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Openness
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateOpenness(
	float Current, float Target) const
{
	if (Target > Current)
	{
		const float Ratio = (Target - Current) / FMath::Max(100.f - Current, 1.f);
		return GenerateOpennessIncrease(Ratio);
	}
	else
	{
		const float Ratio = (Current - Target) / FMath::Max(Current, 1.f);
		return GenerateOpennessDecrease(Ratio);
	}
}

TArray<FEditLayer> USliderLayerGenerator::GenerateOpennessIncrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::Openness);

	TArray<FBuildingInfo> Buildings = GetBuildingInfos();
	AnalyzeBlockingDirs(Buildings);

	// 시야 차단 건물 (5/8방향 이상 차단) 추출, 높이 낮은 순 정렬
	TArray<FBuildingInfo> Blockers;
	for (const FBuildingInfo& B : Buildings)
	{
		if (B.BlockingDirCount >= 5)
			Blockers.Add(B);
	}

	Blockers.Sort([](const FBuildingInfo& A, const FBuildingInfo& B)
	{
		return A.HeightM < B.HeightM;
	});

	const int32 Count = FMath::CeilToInt(Blockers.Num() * Ratio);
	const int32 ActualCount = FMath::Min(Count, Blockers.Num());

	for (int32 i = 0; i < ActualCount; ++i)
	{
		Result.Add(MakeBuildingRemoveLayer(Blockers[i], SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("개방도 증가: %d/%d 시야 차단 건물 제거 (비율 %.1f%%)"),
		ActualCount, Blockers.Num(), Ratio * 100.f);
	return Result;
}

TArray<FEditLayer> USliderLayerGenerator::GenerateOpennessDecrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::Openness);

	// 시야가 탁 트인 셀(8방향 모두 비차단) → 건물 추가
	if (!Subsystem) return Result;

	const float RadiusCm   = Subsystem->MapRadiusKm * 100000.f;
	const float GridCellCm = 20000.f;
	const float CheckRange = 20000.f;
	const float MinBlockH  = 3.f;

	TArray<FBuildingInfo> Buildings = GetBuildingInfos();

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);
	TArray<FVector2D> OpenCells;

	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D Cell(gx * GridCellCm, gy * GridCellCm);
			if (Cell.Size() > RadiusCm) continue;

			int32 BlockedCount = 0;
			for (int32 d = 0; d < 8; ++d)
			{
				for (const FBuildingInfo& B : Buildings)
				{
					if (B.HeightM < MinBlockH) continue;
					const FVector2D Delta = B.CentroidUE5 - Cell;
					const float Dist = Delta.Size();
					if (Dist < 1.f || Dist > CheckRange) continue;
					const float Dot = FVector2D::DotProduct(Delta / Dist, kDirections8[d]);
					if (Dot > kCosConeHalf) { BlockedCount++; break; }
				}
			}

			if (BlockedCount <= 1) OpenCells.Add(Cell);
		}
	}

	const int32 Count = FMath::CeilToInt(OpenCells.Num() * Ratio);
	const int32 ActualCount = FMath::Min(Count, OpenCells.Num());

	for (int32 i = 0; i < ActualCount; ++i)
	{
		const float HeightM = 10.f + FMath::FRandRange(0.f, 10.f);
		Result.Add(MakeBuildingAddLayer(OpenCells[i], HeightM, SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("개방도 감소: %d/%d 개방 셀에 건물 추가 (비율 %.1f%%)"),
		ActualCount, OpenCells.Num(), Ratio * 100.f);
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Analysis helpers
// ─────────────────────────────────────────────────────────────────────────────

TArray<USliderLayerGenerator::FBuildingInfo> USliderLayerGenerator::GetBuildingInfos() const
{
	TArray<FBuildingInfo> Infos;
	if (!Subsystem) return Infos;

	for (const auto& B : Subsystem->CachedBuildings)
	{
		FBuildingInfo Info;
		Info.OsmId       = B.OsmId;
		Info.CentroidUE5 = B.CentroidUE5;
		Info.HeightM     = B.HeightM;
		Info.AreaM2      = B.AreaM2;
		Info.TypeKey     = B.TypeKey;
		Info.StableId    = FString::Printf(TEXT("bldg_osm_%lld"), B.OsmId);
		Infos.Add(MoveTemp(Info));
	}
	return Infos;
}

void USliderLayerGenerator::AnalyzeBlockingDirs(TArray<FBuildingInfo>& Buildings) const
{
	if (!Subsystem) return;

	const float RadiusCm    = Subsystem->MapRadiusKm * 100000.f;
	const float GridCellCm  = 20000.f;
	const float CheckRange  = 20000.f;
	const float MinBlockH   = 3.f;

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);

	// Per-building: count how many grid-cell directions this building blocks
	for (FBuildingInfo& B : Buildings)
	{
		if (B.HeightM < MinBlockH) continue;
		B.BlockingDirCount = 0;
	}

	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D Cell(gx * GridCellCm, gy * GridCellCm);
			if (Cell.Size() > RadiusCm) continue;

			for (int32 d = 0; d < 8; ++d)
			{
				for (FBuildingInfo& B : Buildings)
				{
					if (B.HeightM < MinBlockH) continue;
					const FVector2D Delta = B.CentroidUE5 - Cell;
					const float Dist = Delta.Size();
					if (Dist < 1.f || Dist > CheckRange) continue;
					const float Dot = FVector2D::DotProduct(Delta / Dist, kDirections8[d]);
					if (Dot > kCosConeHalf)
					{
						B.BlockingDirCount++;
						break;
					}
				}
			}
		}
	}
}

TArray<FVector2D> USliderLayerGenerator::FindEmptyCells() const
{
	TArray<FVector2D> Result;
	if (!Subsystem) return Result;

	const float RadiusCm   = Subsystem->MapRadiusKm * 100000.f;
	const float GridCellCm = 5000.f;  // 50m cells for building placement
	const float MinDistCm  = 3000.f;  // min 30m between buildings

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);

	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D Cell(gx * GridCellCm, gy * GridCellCm);
			if (Cell.Size() > RadiusCm) continue;

			bool bOccupied = false;
			for (const auto& B : Subsystem->CachedBuildings)
			{
				if (FVector2D::Distance(B.CentroidUE5, Cell) < MinDistCm)
				{
					bOccupied = true;
					break;
				}
			}

			if (!bOccupied) Result.Add(Cell);
		}
	}
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RouteComplexity
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateRouteComplexity(
	float Current, float Target) const
{
	if (Target > Current)
	{
		const float Ratio = (Target - Current) / FMath::Max(100.f - Current, 1.f);
		return GenerateRouteIncrease(Ratio);
	}
	else
	{
		const float Ratio = (Current - Target) / FMath::Max(Current, 1.f);
		return GenerateRouteDecrease(Ratio);
	}
}

TArray<FEditLayer> USliderLayerGenerator::GenerateRouteIncrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::RouteComplexity);

	TMap<int64, FRoadNodeInfo> Nodes = BuildRoadGraph();
	if (Nodes.Num() == 0) return Result;

	// degree-2 이상 노드(교차로) 쌍 탐색 → 우회로 없는 구간에 road_add
	TArray<FRoadNodeInfo> Intersections;
	for (const auto& KV : Nodes)
	{
		if (KV.Value.Degree >= 2)
			Intersections.Add(KV.Value);
	}

	// degree 낮은 순 정렬 (연결이 적은 교차로부터 우회로 추가)
	Intersections.Sort([](const FRoadNodeInfo& A, const FRoadNodeInfo& B)
	{
		return A.Degree < B.Degree;
	});

	const int32 Count = FMath::CeilToInt(Intersections.Num() * Ratio * 0.5f);
	const int32 ActualCount = FMath::Min(Count, Intersections.Num() / 2);

	for (int32 i = 0; i + 1 < ActualCount * 2 && i + 1 < Intersections.Num(); i += 2)
	{
		const FVector2D& A = Intersections[i].Location;
		const FVector2D& B = Intersections[i + 1].Location;

		const FVector2D Mid = (A + B) * 0.5f;
		const FVector2D Perp = (B - A).GetRotated(90.f).GetSafeNormal() * 3000.f;
		const FVector2D Detour = Mid + Perp;

		Result.Add(MakeRoadAddLayer(A, Detour, 4.f, SourceId));
		Result.Add(MakeRoadAddLayer(Detour, B, 4.f, SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("동선 복잡도 증가: %d 우회 도로 레이어 생성"),
		Result.Num());
	return Result;
}

TArray<FEditLayer> USliderLayerGenerator::GenerateRouteDecrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::RouteComplexity);

	TMap<int64, FRoadNodeInfo> Nodes = BuildRoadGraph();
	if (Nodes.Num() == 0) return Result;

	// degree-1 노드(막다른 길) → road_block
	TArray<FRoadNodeInfo> DeadEnds;
	for (const auto& KV : Nodes)
	{
		if (KV.Value.Degree == 1)
			DeadEnds.Add(KV.Value);
	}

	const int32 Count = FMath::CeilToInt(DeadEnds.Num() * Ratio);
	const int32 ActualCount = FMath::Min(Count, DeadEnds.Num());

	for (int32 i = 0; i < ActualCount; ++i)
	{
		Result.Add(MakeRoadBlockLayer(DeadEnds[i].Location, SourceId));
	}

	UE_LOG(LogSliderGen, Log, TEXT("동선 복잡도 감소: %d 도로 차단 레이어 생성"),
		ActualCount);
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ElevationContrast
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateElevationContrast(
	float Current, float Target) const
{
	if (Target > Current)
	{
		const float Ratio = (Target - Current) / FMath::Max(100.f - Current, 1.f);
		return GenerateElevationIncrease(Ratio);
	}
	else
	{
		const float Ratio = (Current - Target) / FMath::Max(Current, 1.f);
		return GenerateElevationDecrease(Ratio);
	}
}

TArray<FEditLayer> USliderLayerGenerator::GenerateElevationIncrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	if (!Subsystem) return Result;
	const FString SourceId = MakeSourceId(ESliderType::ElevationContrast);

	const float ElevRange = Subsystem->ElevationMaxM - Subsystem->ElevationMinM;
	if (ElevRange <= 0.f) return Result;

	// max_delta = (최고-최저) × 0.05, scaled by ratio
	const float MaxDeltaM = ElevRange * 0.05f;
	const float DeltaM    = MaxDeltaM * Ratio;
	const float DeltaCm   = DeltaM * 100.f;

	const float RadiusCm   = Subsystem->MapRadiusKm * 100000.f;
	const float GridCellCm = 20000.f;
	const float CellRadiusCm = GridCellCm * 0.5f;

	const float AvgElevM = (Subsystem->ElevationMinM + Subsystem->ElevationMaxM) * 0.5f;

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);

	// 200m 그리드 샘플: 평균 이상 → +delta, 평균 이하 → -delta
	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D Cell(gx * GridCellCm, gy * GridCellCm);
			if (Cell.Size() > RadiusCm) continue;

			// approximate elevation from position relative to map center:
			// higher cells are further from center in elevation range distribution
			const float NormDist = Cell.Size() / RadiusCm;
			const float EstElevM = FMath::Lerp(Subsystem->ElevationMaxM,
				Subsystem->ElevationMinM, NormDist);

			const float Sign = (EstElevM >= AvgElevM) ? 1.f : -1.f;
			Result.Add(MakeTerrainModifyLayer(Cell, CellRadiusCm,
				TEXT("offset"), Sign * DeltaCm, 0.8f, SourceId));
		}
	}

	UE_LOG(LogSliderGen, Log,
		TEXT("고저차 증가: %d 지형 수정 레이어 (delta ±%.1fcm)"),
		Result.Num(), DeltaCm);
	return Result;
}

TArray<FEditLayer> USliderLayerGenerator::GenerateElevationDecrease(float Ratio) const
{
	TArray<FEditLayer> Result;
	if (!Subsystem) return Result;
	const FString SourceId = MakeSourceId(ESliderType::ElevationContrast);

	const float RadiusCm   = Subsystem->MapRadiusKm * 100000.f;
	const float GridCellCm = 20000.f;
	const float CellRadiusCm = GridCellCm * 0.5f;

	const float AvgElevCm = (Subsystem->ElevationMinM + Subsystem->ElevationMaxM) * 0.5f * 100.f;

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);

	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D Cell(gx * GridCellCm, gy * GridCellCm);
			if (Cell.Size() > RadiusCm) continue;

			Result.Add(MakeTerrainModifyLayer(Cell, CellRadiusCm,
				TEXT("flatten"), AvgElevCm, Ratio, SourceId));
		}
	}

	UE_LOG(LogSliderGen, Log,
		TEXT("고저차 감소: %d 지형 평탄화 레이어 (target %.0fcm, strength %.2f)"),
		Result.Num(), AvgElevCm, Ratio);
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DestructionLevel
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer> USliderLayerGenerator::GenerateDestructionLevel(
	float Current, float Target) const
{
	TArray<FEditLayer> Result;
	const FString SourceId = MakeSourceId(ESliderType::DestructionLevel);

	TArray<FBuildingInfo> Buildings = GetBuildingInfos();
	if (Buildings.Num() == 0) return Result;

	// 무작위 셔플
	for (int32 i = Buildings.Num() - 1; i > 0; --i)
	{
		const int32 j = FMath::RandRange(0, i);
		Buildings.Swap(i, j);
	}

	const float TargetFrac = Target / 100.f;
	const int32 TargetCount = FMath::RoundToInt(Buildings.Num() * TargetFrac);

	if (Target > Current)
	{
		// intact → partial → destroyed 순서로 승격
		int32 Applied = 0;
		for (int32 i = 0; i < Buildings.Num() && Applied < TargetCount; ++i)
		{
			const FString State = (Applied < TargetCount / 2) ? TEXT("partial") : TEXT("destroyed");
			Result.Add(MakeDestructionLayer(Buildings[i], State, SourceId));
			Applied++;
		}
	}
	else
	{
		// destroyed → partial → intact 순서로 강등 (= 파괴 건물 복원)
		// 현재 파괴된 건물 중 복원할 비율
		const float CurrentFrac = Current / 100.f;
		const int32 CurrentCount = FMath::RoundToInt(Buildings.Num() * CurrentFrac);
		const int32 RestoreCount = CurrentCount - TargetCount;

		for (int32 i = 0; i < FMath::Min(RestoreCount, Buildings.Num()); ++i)
		{
			Result.Add(MakeDestructionLayer(Buildings[i], TEXT("intact"), SourceId));
		}
	}

	UE_LOG(LogSliderGen, Log, TEXT("파괴도 %.0f→%.0f: %d 파괴 상태 레이어 생성"),
		Current, Target, Result.Num());
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Road graph helper
// ─────────────────────────────────────────────────────────────────────────────

TMap<int64, USliderLayerGenerator::FRoadNodeInfo> USliderLayerGenerator::BuildRoadGraph() const
{
	TMap<int64, FRoadNodeInfo> Nodes;
	if (!Subsystem) return Nodes;

	const float SnapCellCm = 1000.f;

	auto CellKey = [SnapCellCm](const FVector2D& Pt) -> int64
	{
		int32 cx = FMath::FloorToInt(Pt.X / SnapCellCm);
		int32 cy = FMath::FloorToInt(Pt.Y / SnapCellCm);
		return (static_cast<int64>(cx) << 32) | static_cast<int64>(static_cast<uint32>(cy));
	};

	for (const auto& Road : Subsystem->CachedRoads)
	{
		if (Road.PointsUE5.Num() < 2) continue;

		int64 PrevKey = -1;
		for (int32 i = 0; i < Road.PointsUE5.Num(); ++i)
		{
			const int64 Key = CellKey(Road.PointsUE5[i]);
			FRoadNodeInfo& Node = Nodes.FindOrAdd(Key);
			Node.CellKey  = Key;
			Node.Location = Road.PointsUE5[i];

			if (PrevKey != -1 && PrevKey != Key)
			{
				Node.Degree++;
				Nodes.FindOrAdd(PrevKey).Degree++;
			}
			PrevKey = Key;
		}
	}

	return Nodes;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Layer factories
// ─────────────────────────────────────────────────────────────────────────────

FEditLayer USliderLayerGenerator::MakeBuildingRemoveLayer(
	const FBuildingInfo& Bldg, const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::BuildingRemove;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;
	L.Label     = FString::Printf(TEXT("[슬라이더] 건물 제거 %s"), *Bldg.StableId);

	L.Area.Type = EAreaType::ActorRef;
	L.Area.ActorRef.StableId = Bldg.StableId;
	L.Area.ActorRef.FallbackLocationUE5 = FVector(Bldg.CentroidUE5.X, Bldg.CentroidUE5.Y, 0.f);

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetBoolField(TEXT("keep_foundation"), true);

	return L;
}

FEditLayer USliderLayerGenerator::MakeBuildingAddLayer(
	const FVector2D& Location, float HeightM, const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::BuildingAdd;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;
	L.Label     = FString::Printf(TEXT("[슬라이더] 건물 추가 (%.0f, %.0f)"),
		Location.X / 100.f, Location.Y / 100.f);

	L.Area.Type = EAreaType::Point;
	L.Area.Point.LocationUE5 = FVector(Location.X, Location.Y, 0.f);

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetStringField(TEXT("type_key"), TEXT("Residential_Mid"));
	L.Params->SetNumberField(TEXT("footprint_m2"), 100.0);
	L.Params->SetNumberField(TEXT("aspect_ratio"), 1.2);
	L.Params->SetNumberField(TEXT("height_m"), HeightM);
	L.Params->SetNumberField(TEXT("rotation_deg"),
		FMath::FRandRange(0.f, 360.f));
	L.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));

	return L;
}

FEditLayer USliderLayerGenerator::MakeRoadAddLayer(
	const FVector2D& Start, const FVector2D& End, float WidthM,
	const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::RoadAdd;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;
	L.Label     = FString::Printf(TEXT("[슬라이더] 우회 도로 (%.0f,%.0f)→(%.0f,%.0f)"),
		Start.X / 100.f, Start.Y / 100.f, End.X / 100.f, End.Y / 100.f);

	L.Area.Type = EAreaType::Path;
	L.Area.Path.PointsUE5 = { Start, End };
	L.Area.Path.WidthCm   = WidthM * 100.f;

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetStringField(TEXT("road_type"), TEXT("minor"));
	L.Params->SetNumberField(TEXT("width_m"), WidthM);

	return L;
}

FEditLayer USliderLayerGenerator::MakeRoadBlockLayer(
	const FVector2D& Location, const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::RoadBlock;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;
	L.Label     = FString::Printf(TEXT("[슬라이더] 도로 차단 (%.0f, %.0f)"),
		Location.X / 100.f, Location.Y / 100.f);

	L.Area.Type = EAreaType::Point;
	L.Area.Point.LocationUE5 = FVector(Location.X, Location.Y, 0.f);

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetStringField(TEXT("obstacle_type"), TEXT("barricade"));
	L.Params->SetBoolField(TEXT("passable_on_foot"), true);

	return L;
}

FEditLayer USliderLayerGenerator::MakeTerrainModifyLayer(
	const FVector2D& Center, float RadiusCm, const FString& Operation,
	float DeltaCm, float Strength, const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::TerrainModify;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;

	if (Operation == TEXT("flatten"))
	{
		L.Label = FString::Printf(TEXT("[슬라이더] 지형 평탄화 (%.0f, %.0f)"),
			Center.X / 100.f, Center.Y / 100.f);
	}
	else
	{
		L.Label = FString::Printf(TEXT("[슬라이더] 지형 %s%.0fcm (%.0f, %.0f)"),
			DeltaCm >= 0.f ? TEXT("+") : TEXT(""), DeltaCm,
			Center.X / 100.f, Center.Y / 100.f);
	}

	L.Area.Type = EAreaType::Circle;
	L.Area.Circle.CenterUE5 = Center;
	L.Area.Circle.RadiusCm  = RadiusCm;

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetStringField(TEXT("operation"), Operation);
	L.Params->SetStringField(TEXT("falloff"), TEXT("smooth"));
	L.Params->SetNumberField(TEXT("strength"), Strength);

	if (Operation == TEXT("flatten"))
	{
		L.Params->SetNumberField(TEXT("target_height_cm"), DeltaCm);
	}
	else
	{
		L.Params->SetNumberField(TEXT("height_delta_cm"), DeltaCm);
	}

	return L;
}

FEditLayer USliderLayerGenerator::MakeDestructionLayer(
	const FBuildingInfo& Bldg, const FString& NewState,
	const FString& SourceId)
{
	FEditLayer L;
	L.Type      = EEditLayerType::DestructionState;
	L.CreatedBy = ELayerCreatedBy::Slider;
	L.SourceId  = SourceId;
	L.Label     = FString::Printf(TEXT("[슬라이더] 파괴 %s → %s"),
		*Bldg.StableId, *NewState);

	L.Area.Type = EAreaType::ActorRef;
	L.Area.ActorRef.StableId = Bldg.StableId;
	L.Area.ActorRef.FallbackLocationUE5 = FVector(Bldg.CentroidUE5.X, Bldg.CentroidUE5.Y, 0.f);

	L.Params = MakeShared<FJsonObject>();
	L.Params->SetStringField(TEXT("new_state"), NewState);

	return L;
}
