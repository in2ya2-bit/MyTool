#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "DesignerIntentTypes.h"
#include "EditLayerTypes.h"
#include "SliderLayerGenerator.generated.h"

class UEditLayerManager;
class UDesignerIntentSubsystem;

/**
 * USliderLayerGenerator
 *
 * 슬라이더 값 변경 → Edit Layer 배열 생성 엔진.
 * 각 슬라이더 타입별로 "현재값 → 목표값" 변환에 필요한
 * FEditLayer 배열을 산출한다.
 *
 * 적용 순서: ① 고저차 → ② 파괴도 → ③ 도심 밀도 → ④ 개방도 → ⑤ 동선 복잡도
 */
UCLASS()
class LEVELTOOL_API USliderLayerGenerator : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UDesignerIntentSubsystem* InSubsystem);

	/**
	 * 슬라이더 값 변경에 대응하는 Edit Layer 배열을 생성.
	 * 기존 해당 슬라이더 소스의 레이어는 호출자가 교체(ReplaceLayersBySourceId).
	 */
	TArray<FEditLayer> GenerateLayers(ESliderType Type, float CurrentValue,
	                                  float NewValue) const;

	/** 슬라이더 SourceId 생성 (ReplaceLayersBySourceId 키) */
	static FString MakeSourceId(ESliderType Type);

	/** 경량 시뮬레이션 — 정규 Generate 없이 예상 레이어 수만 산출 */
	struct FLightweightEstimate
	{
		int32 AddCount = 0;
		int32 RemoveCount = 0;
		float EstimatedAchievementPct = 100.f;
	};
	FLightweightEstimate EstimateLightweight(ESliderType Type, float CurrentValue, float NewValue) const;

private:
	// ── UrbanDensity ────────────────────────────────────────────────────
	TArray<FEditLayer> GenerateUrbanDensity(float Current, float Target) const;
	TArray<FEditLayer> GenerateDensityIncrease(float Ratio) const;
	TArray<FEditLayer> GenerateDensityDecrease(float Ratio) const;

	// ── Openness ────────────────────────────────────────────────────────
	TArray<FEditLayer> GenerateOpenness(float Current, float Target) const;
	TArray<FEditLayer> GenerateOpennessIncrease(float Ratio) const;
	TArray<FEditLayer> GenerateOpennessDecrease(float Ratio) const;

	// ── RouteComplexity ─────────────────────────────────────────────────
	TArray<FEditLayer> GenerateRouteComplexity(float Current, float Target) const;
	TArray<FEditLayer> GenerateRouteIncrease(float Ratio) const;
	TArray<FEditLayer> GenerateRouteDecrease(float Ratio) const;

	// ── ElevationContrast ───────────────────────────────────────────────
	TArray<FEditLayer> GenerateElevationContrast(float Current, float Target) const;
	TArray<FEditLayer> GenerateElevationIncrease(float Ratio) const;
	TArray<FEditLayer> GenerateElevationDecrease(float Ratio) const;

	// ── DestructionLevel ────────────────────────────────────────────────
	TArray<FEditLayer> GenerateDestructionLevel(float Current, float Target) const;

	// ── 분석 헬퍼 ──────────────────────────────────────────────────────
	struct FBuildingInfo
	{
		int64     OsmId;
		FVector2D CentroidUE5;
		float     HeightM;
		float     AreaM2;
		FString   TypeKey;
		FString   StableId;
		int32     BlockingDirCount = 0;
	};

	struct FRoadNodeInfo
	{
		int64     CellKey;
		FVector2D Location;
		int32     Degree = 0;
	};

	TArray<FBuildingInfo> GetBuildingInfos() const;
	void AnalyzeBlockingDirs(TArray<FBuildingInfo>& Buildings) const;
	TArray<FVector2D> FindEmptyCells() const;
	TMap<int64, FRoadNodeInfo> BuildRoadGraph() const;

	// ── 레이어 팩토리 ──────────────────────────────────────────────────
	static FEditLayer MakeBuildingRemoveLayer(const FBuildingInfo& Bldg,
	                                          const FString& SourceId);
	static FEditLayer MakeBuildingAddLayer(const FVector2D& Location,
	                                       float HeightM, const FString& SourceId);
	static FEditLayer MakeRoadAddLayer(const FVector2D& Start, const FVector2D& End,
	                                   float WidthM, const FString& SourceId);
	static FEditLayer MakeRoadBlockLayer(const FVector2D& Location,
	                                     const FString& SourceId);
	static FEditLayer MakeTerrainModifyLayer(const FVector2D& Center, float RadiusCm,
	                                         const FString& Operation, float DeltaCm,
	                                         float Strength, const FString& SourceId);
	static FEditLayer MakeDestructionLayer(const FBuildingInfo& Bldg,
	                                       const FString& NewState,
	                                       const FString& SourceId);

	UPROPERTY()
	TObjectPtr<UDesignerIntentSubsystem> Subsystem;
};
