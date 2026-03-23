#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "EditLayerTypes.h"
#include "EditLayerApplicator.generated.h"

class UEditLayerManager;
class ULevelToolBuildingPool;
class ALandscapeProxy;
class ULandscapeInfo;

/**
 * UEditLayerApplicator
 *
 * Edit Layer → Actor 세계 반영 엔진.
 * Apply(생성/보이기), Hide(숨기기), Delete(영구 제거) 3가지 시나리오를
 * 레이어 타입별로 처리한다.
 *
 * 모든 조작은 FScopedTransaction으로 감싸서 Ctrl+Z 지원.
 */
UCLASS()
class LEVELTOOL_API UEditLayerApplicator : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UEditLayerManager* InLayerManager);

	// ── 핵심 3가지 동작 ─────────────────────────────────────────────────
	void ApplyLayer(const FString& LayerId);
	void HideLayer(const FString& LayerId);
	void DeleteLayer(const FString& LayerId);

	// ── stable_id → Actor 해석 ──────────────────────────────────────────
	FResolveResult ResolveStableId(const FString& StableId,
	                               const FVector& FallbackLocation = FVector::ZeroVector) const;

	// ── 유틸리티 ────────────────────────────────────────────────────────
	AActor* FindActorByStableId(const FString& StableId) const;

private:
	// 타입별 Apply 분기
	void ApplyTerrainModify(struct FEditLayer& Layer);
	void ApplyBuildingAdd(struct FEditLayer& Layer);
	void ApplyBuildingRemove(struct FEditLayer& Layer);
	void ApplyBuildingHeight(struct FEditLayer& Layer);
	void ApplyDestructionState(struct FEditLayer& Layer);
	void ApplyRoadBlock(struct FEditLayer& Layer);
	void ApplyRoadAdd(struct FEditLayer& Layer);
	void ApplyRoadWidth(struct FEditLayer& Layer);
	void ApplyMarker(struct FEditLayer& Layer);
	void ApplyBuildingAddBatch(struct FEditLayer& Layer);

	// 타입별 Hide 분기
	void HideBuildingAdd(struct FEditLayer& Layer);
	void HideBuildingRemove(struct FEditLayer& Layer);
	void HideBuildingHeight(struct FEditLayer& Layer);
	void HideSpawnedActors(const FString& LayerId);

	// 타입별 Delete 분기
	void DeleteSpawnedActors(const FString& LayerId);
	void RestoreOriginalActor(struct FEditLayer& Layer);

	// ── terrain_modify: Full Replay ─────────────────────────────────────
	ALandscapeProxy* FindLandscape() const;
	void CaptureTerrainBase(ALandscapeProxy* Landscape, ULandscapeInfo* Info);
	void RestoreTerrainBase(ULandscapeInfo* Info);
	void FullReplayTerrainLayers();
	void ApplyTerrainModifySingle(const struct FEditLayer& Layer, ALandscapeProxy* Landscape, ULandscapeInfo* Info);

	bool bTerrainBaseCaptured = false;
	TArray<uint16> TerrainBaseHeightmap;
	int32 TerrainBaseMinX = 0, TerrainBaseMinY = 0;
	int32 TerrainBaseMaxX = 0, TerrainBaseMaxY = 0;

	void SaveTerrainBaseToDisk();
	bool LoadTerrainBaseFromDisk();

	// ── building_add: 배치 충돌 해소 ────────────────────────────────────
	bool CheckPlacementCollision(const FVector& Location, float FootprintCm, const FString& SkipLayerId) const;
	bool CheckSlopeAt(float WorldX, float WorldY, float MaxSlopeDeg = 30.f) const;
	bool CheckWaterAt(float WorldX, float WorldY) const;
	FVector FindValidPlacement(const FVector& Desired, float FootprintCm, const FString& SkipLayerId) const;

	// G-8: 지형 수정 후 건물/도로 Z 동기화
	void SyncActorZInArea(const FVector2D& Center, float RadiusCm, ALandscapeProxy* Landscape);

	// Actor 태그 부여
	void TagActor(AActor* Actor, const FString& StableId) const;

	// stable_id 생성 (2단계 추가 건물/도로)
	static FString GenerateStableId(const struct FEditLayer& Layer);

	// Heightmap Z 조회 (캐시된 높이맵 기반)
	float GetTerrainZ(float WorldX, float WorldY) const;

	UPROPERTY()
	TObjectPtr<UEditLayerManager> LayerManager;

};
