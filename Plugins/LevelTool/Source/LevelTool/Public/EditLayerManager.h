#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "EditLayerTypes.h"
#include "EditLayerManager.generated.h"

class UEditLayerApplicator;

DECLARE_MULTICAST_DELEGATE(FOnLayersChanged);

UCLASS()
class LEVELTOOL_API UEditLayerManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(const FString& InMapId);

	// ── CRUD ────────────────────────────────────────────────────────────
	FString  AddLayer(FEditLayer&& Layer);
	bool     RemoveLayer(const FString& LayerId);
	FEditLayer*       FindLayer(const FString& LayerId);
	const FEditLayer* FindLayer(const FString& LayerId) const;
	const TArray<FEditLayer>& GetAllLayers() const { return Layers; }

	// ── Visibility / Lock ───────────────────────────────────────────────
	bool SetLayerVisible(const FString& LayerId, bool bVisible);
	bool ToggleLayerVisibility(const FString& LayerId);
	bool SetLayerLocked(const FString& LayerId, bool bLocked);

	// ── Ordering ────────────────────────────────────────────────────────
	bool ReorderLayer(const FString& LayerId, int32 NewIndex);

	// ── Query ───────────────────────────────────────────────────────────
	TArray<FEditLayer*> FindLayersBySourceId(const FString& SourceId);
	TArray<FEditLayer*> FindLayersByCreatedBy(ELayerCreatedBy CreatedBy);
	int32 GetLayerCount() const { return Layers.Num(); }

	// ── Replacement policy (동일 슬라이더 재조정 → 이전 세트 교체) ────
	void ReplaceLayersBySourceId(const FString& SourceId, TArray<FEditLayer>&& NewLayers);

	// ── 배치 ↔ 개별 전환 ──────────────────────────────────────────────
	int32 ExplodeBatchLayer(const FString& BatchLayerId);
	FString MergeToBatchLayer(const TArray<FString>& LayerIds, const FString& Label);

	// ── 교차 레이어 충돌 감지 ──────────────────────────────────────────
	struct FLayerConflict
	{
		FString LayerIdA;
		FString LayerIdB;
		FString Reason;
	};
	TArray<FLayerConflict> DetectLayerConflicts() const;

	// ── Serialization ───────────────────────────────────────────────────
	bool SaveToJson() const;
	bool LoadFromJson();

	// ── Layer-Actor mapping (runtime, not persisted) ────────────────────
	void MapActorToLayer(const FString& LayerId, AActor* Actor);
	void UnmapActorsForLayer(const FString& LayerId);
	TArray<TWeakObjectPtr<AActor>> GetMappedActors(const FString& LayerId) const;

	// ── Applicator (Actor 반영 엔진) ────────────────────────────────────
	UEditLayerApplicator* GetApplicator() const { return Applicator; }

	// ── Accessors ───────────────────────────────────────────────────────
	const FString& GetMapId() const { return MapId; }
	void  SetMapId(const FString& InMapId) { MapId = InMapId; }
	FString GetLayersJsonPath() const;

	// ── Events ──────────────────────────────────────────────────────────
	FOnLayersChanged OnLayersChanged;

private:
	void BroadcastChange();
	static FString GenerateLayerId();

	// JSON conversion
	TSharedPtr<FJsonObject> LayerToJson(const FEditLayer& Layer) const;
	bool JsonToLayer(const TSharedPtr<FJsonObject>& JsonObj, FEditLayer& OutLayer) const;
	TSharedPtr<FJsonObject> AreaToJson(const FEditLayerArea& Area) const;
	bool JsonToArea(const TSharedPtr<FJsonObject>& JsonObj, FEditLayerArea& OutArea) const;

	// Enum ↔ String
	static FString          LayerTypeToString(EEditLayerType Type);
	static EEditLayerType   StringToLayerType(const FString& Str);
	static FString          AreaTypeToString(EAreaType Type);
	static EAreaType        StringToAreaType(const FString& Str);
	static FString          CreatedByToString(ELayerCreatedBy CreatedBy);
	static ELayerCreatedBy  StringToCreatedBy(const FString& Str);

	FString MapId;
	TArray<FEditLayer> Layers;
	TMap<FString, TArray<TWeakObjectPtr<AActor>>> LayerActorMap;

	UPROPERTY()
	TObjectPtr<UEditLayerApplicator> Applicator;
};
