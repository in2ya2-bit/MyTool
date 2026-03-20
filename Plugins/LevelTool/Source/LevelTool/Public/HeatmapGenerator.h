#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "HeatmapTypes.h"
#include "HeatmapGenerator.generated.h"

class UDesignerIntentSubsystem;
class ADecalActor;
class UMaterialInstanceDynamic;

/**
 * UHeatmapGenerator
 *
 * 체크 결과를 히트맵 텍스처/데이터로 변환.
 * 50m 격자 기반, Raycast 없이 JSON 데이터 분석만 수행.
 * 뷰포트에 ADecalActor 기반 오버레이 투영 지원.
 */
UCLASS()
class LEVELTOOL_API UHeatmapGenerator : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UDesignerIntentSubsystem* InSubsystem);

	FHeatmapData Generate(EHeatmapMode Mode) const;
	const FHeatmapData& GetCachedHeatmap(EHeatmapMode Mode) const;

	void GenerateAll();
	bool HasCachedData() const { return CachedMaps.Num() > 0; }

	UTexture2D* RenderToTexture(const FHeatmapData& Data) const;

	// ── 뷰포트 데칼 오버레이 ────────────────────────────────────────────
	void ShowHeatmapOverlay(EHeatmapMode Mode);
	void HideHeatmapOverlay();
	bool IsOverlayVisible() const { return ActiveDecal.IsValid(); }
	EHeatmapMode GetActiveOverlayMode() const { return ActiveOverlayMode; }

private:
	FHeatmapData GenerateBuildingDensity() const;
	FHeatmapData GenerateRoadConnectivity() const;
	FHeatmapData GeneratePoiDistribution() const;
	FHeatmapData GenerateElevation() const;

	FLinearColor ValueToColor_RedGreen(float V) const;
	FLinearColor ValueToColor_Elevation(float V) const;

	int32 GridHalf() const;
	float CellSize() const;

	UPROPERTY()
	TObjectPtr<UDesignerIntentSubsystem> Subsystem;

	TMap<EHeatmapMode, FHeatmapData> CachedMaps;
	static const FHeatmapData EmptyHeatmap;

	UMaterial* GetOrCreateDecalMaterial();

	// ── 데칼 오버레이 상태 ──────────────────────────────────────────────
	TWeakObjectPtr<ADecalActor> ActiveDecal;
	EHeatmapMode ActiveOverlayMode = EHeatmapMode::BuildingDensity;

	UPROPERTY()
	TObjectPtr<UTexture2D> ActiveOverlayTexture;

	UPROPERTY()
	TObjectPtr<UMaterial> CachedDecalMaterial;
};
