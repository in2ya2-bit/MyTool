#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "DesignerIntentTypes.h"
#include "DesignerIntentSubsystem.generated.h"

class UEditLayerManager;
class USliderLayerGenerator;
class UPresetManager;
class UChecklistEngine;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnSliderInitialized, const FString& /*MapId*/);

UCLASS()
class LEVELTOOL_API UDesignerIntentSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── 1단계 연동 ──────────────────────────────────────────────────────
	void OnStage1Complete(const FString& MapId);
	bool IsStage1Ready() const;
	bool CheckStage1Prerequisites(TArray<FString>& OutMissing) const;

	// ── 슬라이더 초기값 ─────────────────────────────────────────────────
	float GetSliderInitialValue(ESliderType Type) const;
	const FSliderState& GetSliderState(ESliderType Type) const;
	const TMap<ESliderType, FSliderState>& GetAllSliderStates() const { return SliderStates; }

	// ── 2단계: 슬라이더 (S3+ 구현) ──────────────────────────────────────
	void    ApplySlider(ESliderType Type, float NewValue);
	void    BeginBatchApply() { bBatchApply = true; }
	void    EndBatchApply();
	float   GetSliderAchievementRate(ESliderType Type) const;
	FString GetAchievementLimitReason(ESliderType Type) const;

	// ── 레이어 매니저 / 프리셋 / 체크리스트 접근 ────────────────────────
	UEditLayerManager*  GetLayerManager() const;
	UPresetManager*     GetPresetManager() const { return PresetMgr; }
	UChecklistEngine*   GetChecklistEngine() const { return CheckEngine; }

	// ── P1-8: 개별 Actor 단건 편집 ──────────────────────────────────────
	bool SingleEditActor(AActor* Actor, ESingleEditType EditType, float Param = 0.f);

	// ── 맵 ID ───────────────────────────────────────────────────────────
	const FString& GetActiveMapId() const { return ActiveMapId; }

	// ── 내부 분석용 데이터 (getter보다 선행 선언 필요) ───────────────────
	struct FBuildingAnalysis
	{
		int64     OsmId;
		FVector2D CentroidUE5;
		float     HeightM;
		float     AreaM2;
		FString   TypeKey;
	};

	struct FRoadAnalysis
	{
		int64     OsmId;
		FString   RoadType;
		TArray<FVector2D> PointsUE5;
	};

	struct FWaterArea { TArray<FVector2D> Polygon; FVector2D Center; float RadiusCm; };

	// ── 캐시 데이터 접근 (Applicator/ChecklistEngine 외부 참조용) ─────
	const TArray<FBuildingAnalysis>& GetCachedBuildings() const { return CachedBuildings; }
	const TArray<FRoadAnalysis>& GetCachedRoads() const { return CachedRoads; }
	const TArray<FWaterArea>& GetCachedWaterAreas() const { return CachedWaterAreas; }
	float GetMapRadiusKm() const { return MapRadiusKm; }
	float GetElevationMinM() const { return ElevationMinM; }
	float GetElevationMaxM() const { return ElevationMaxM; }

	// ── G-7: EffectiveState 재구성 (진단 전 호출) ──────────────────────
	void RebuildEffectiveState();

	// ── P1-10: 맵 규모별 파라미터 스케일링 ──────────────────────────────
	float GetScaledDBSCAN_Eps() const;
	float GetScaledGridCellCm() const;
	float GetScaledMinPoiSpacing() const;

	// ── P2-20: 예측 오차 보정 ───────────────────────────────────────────
	void RecordPredictionSample(float Predicted, float Actual);
	float GetCorrectionBias() const { return PredictionBias; }
	float GetCorrectionScale() const { return PredictionScale; }
	float PredictionBias  = 0.f;
	float PredictionScale = 1.f;

	struct FPredictionSample
	{
		float Predicted;
		float Actual;
		float Error;
		FDateTime Timestamp;
	};
	TArray<FPredictionSample> PredictionHistory;
	void SavePredictionHistoryToMeta() const;
	void LoadPredictionHistoryFromMeta();

	float GetPredictionErrorMargin() const;
	float GetCollisionRate() const;

	// ── 델리게이트 ──────────────────────────────────────────────────────
	FOnSliderInitialized OnSliderInitialized;

private:
	// ── 슬라이더 산출 ───────────────────────────────────────────────────
	void  ComputeSliderInitialValues();
	float ComputeUrbanDensity() const;
	float ComputeOpenness() const;
	float ComputeRouteComplexity() const;
	float ComputeElevationContrast() const;
	float ComputeDestructionLevel() const;

	void ComputeAchievementLimits();
	void SaveSliderValuesToMapMeta() const;

	bool LoadMapMetaData(const FString& MetaPath);
	bool LoadBuildingData(const FString& JsonPath);
	bool LoadRoadData(const FString& JsonPath);
	bool LoadWaterData(const FString& JsonPath);

	void TryRestoreSession();
	FString GetMapMetaPath() const;

	// ── 캐시 데이터 ─────────────────────────────────────────────────────
	FString ActiveMapId;
	float   MapRadiusKm   = 0.f;
	float   ElevationMinM = 0.f;
	float   ElevationMaxM = 0.f;
	FString TerrainType;

	TArray<FBuildingAnalysis> CachedBuildings;
	TArray<FRoadAnalysis>     CachedRoads;
	TArray<FWaterArea> CachedWaterAreas;

	TArray<FBuildingAnalysis> EffectiveBuildings;
	TArray<FRoadAnalysis>     EffectiveRoads;
	TMap<ESliderType, FSliderState> SliderStates;

	static const FSliderState DefaultSliderState;
	bool bBatchApply = false;

	UPROPERTY()
	TObjectPtr<USliderLayerGenerator> LayerGenerator;

	UPROPERTY()
	TObjectPtr<UPresetManager> PresetMgr;

	UPROPERTY()
	TObjectPtr<UChecklistEngine> CheckEngine;

	friend class USliderLayerGenerator;
	friend class UChecklistEngine;
	friend class UHeatmapGenerator;
	friend class UEditLayerApplicator;
};
