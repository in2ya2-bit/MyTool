#include "EditLayerManager.h"
#include "EditLayerApplicator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEditLayer, Log, All);

static constexpr TCHAR kLayersJsonVersion[] = TEXT("3.0");

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerManager::Initialize(const FString& InMapId)
{
	MapId = InMapId;
	Layers.Empty();
	LayerActorMap.Empty();

	if (!Applicator)
	{
		Applicator = NewObject<UEditLayerApplicator>(this);
	}
	Applicator->Initialize(this);

	LoadFromJson();
}

// ─────────────────────────────────────────────────────────────────────────────
//  CRUD
// ─────────────────────────────────────────────────────────────────────────────

FString UEditLayerManager::AddLayer(FEditLayer&& Layer)
{
	if (Layer.LayerId.IsEmpty())
	{
		Layer.LayerId = GenerateLayerId();
	}
	FString Id = Layer.LayerId;
	Layers.Emplace(MoveTemp(Layer));
	BroadcastChange();
	return Id;
}

bool UEditLayerManager::RemoveLayer(const FString& LayerId)
{
	const int32 Idx = Layers.IndexOfByPredicate(
		[&](const FEditLayer& L) { return L.LayerId == LayerId; });
	if (Idx == INDEX_NONE) return false;

	Layers.RemoveAt(Idx);
	LayerActorMap.Remove(LayerId);
	BroadcastChange();
	return true;
}

FEditLayer* UEditLayerManager::FindLayer(const FString& LayerId)
{
	return Layers.FindByPredicate(
		[&](const FEditLayer& L) { return L.LayerId == LayerId; });
}

const FEditLayer* UEditLayerManager::FindLayer(const FString& LayerId) const
{
	return Layers.FindByPredicate(
		[&](const FEditLayer& L) { return L.LayerId == LayerId; });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Visibility / Lock
// ─────────────────────────────────────────────────────────────────────────────

bool UEditLayerManager::SetLayerVisible(const FString& LayerId, bool bVisible)
{
	FEditLayer* L = FindLayer(LayerId);
	if (!L) return false;
	if (L->bVisible == bVisible) return true;
	L->bVisible = bVisible;
	BroadcastChange();
	return true;
}

bool UEditLayerManager::ToggleLayerVisibility(const FString& LayerId)
{
	FEditLayer* L = FindLayer(LayerId);
	if (!L) return false;
	L->bVisible = !L->bVisible;
	BroadcastChange();
	return true;
}

bool UEditLayerManager::SetLayerLocked(const FString& LayerId, bool bLocked)
{
	FEditLayer* L = FindLayer(LayerId);
	if (!L) return false;
	if (L->bLocked == bLocked) return true;
	L->bLocked = bLocked;
	BroadcastChange();
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ordering — NewIndex = desired final position in the resulting array
// ─────────────────────────────────────────────────────────────────────────────

bool UEditLayerManager::ReorderLayer(const FString& LayerId, int32 NewIndex)
{
	const int32 OldIndex = Layers.IndexOfByPredicate(
		[&](const FEditLayer& L) { return L.LayerId == LayerId; });
	if (OldIndex == INDEX_NONE) return false;

	NewIndex = FMath::Clamp(NewIndex, 0, Layers.Num() - 1);
	if (OldIndex == NewIndex) return true;

	FEditLayer Temp = MoveTemp(Layers[OldIndex]);
	Layers.RemoveAt(OldIndex);
	NewIndex = FMath::Clamp(NewIndex, 0, Layers.Num());
	Layers.Insert(MoveTemp(Temp), NewIndex);
	BroadcastChange();
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Query
// ─────────────────────────────────────────────────────────────────────────────

TArray<FEditLayer*> UEditLayerManager::FindLayersBySourceId(const FString& SourceId)
{
	TArray<FEditLayer*> Result;
	for (FEditLayer& L : Layers)
	{
		if (L.SourceId == SourceId) Result.Add(&L);
	}
	return Result;
}

TArray<FEditLayer*> UEditLayerManager::FindLayersByCreatedBy(ELayerCreatedBy CreatedBy)
{
	TArray<FEditLayer*> Result;
	for (FEditLayer& L : Layers)
	{
		if (L.CreatedBy == CreatedBy) Result.Add(&L);
	}
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Replacement policy (동일 슬라이더 재조정 → 이전 세트 교체)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerManager::ReplaceLayersBySourceId(
	const FString& SourceId, TArray<FEditLayer>&& NewLayers)
{
	int32 MaxGen = 0;
	for (const FEditLayer& L : Layers)
	{
		if (L.SourceId == SourceId)
		{
			MaxGen = FMath::Max(MaxGen, L.Generation);
		}
	}

	Layers.RemoveAll([&](const FEditLayer& L) { return L.SourceId == SourceId; });

	for (FEditLayer& L : NewLayers)
	{
		if (L.LayerId.IsEmpty()) L.LayerId = GenerateLayerId();
		L.SourceId   = SourceId;
		L.Generation = MaxGen + 1;
		Layers.Emplace(MoveTemp(L));
	}

	BroadcastChange();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Layer-Actor mapping (runtime, not persisted)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerManager::MapActorToLayer(const FString& LayerId, AActor* Actor)
{
	if (!Actor) return;
	LayerActorMap.FindOrAdd(LayerId).AddUnique(Actor);
}

void UEditLayerManager::UnmapActorsForLayer(const FString& LayerId)
{
	LayerActorMap.Remove(LayerId);
}

TArray<TWeakObjectPtr<AActor>> UEditLayerManager::GetMappedActors(const FString& LayerId) const
{
	const auto* Found = LayerActorMap.Find(LayerId);
	return Found ? *Found : TArray<TWeakObjectPtr<AActor>>();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Path helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UEditLayerManager::GetLayersJsonPath() const
{
	return FPaths::ProjectSavedDir() / TEXT("LevelTool") / TEXT("EditLayers") / MapId / TEXT("layers.json");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerManager::BroadcastChange()
{
	OnLayersChanged.Broadcast();
}

FString UEditLayerManager::GenerateLayerId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Enum ↔ String
// ─────────────────────────────────────────────────────────────────────────────

FString UEditLayerManager::LayerTypeToString(EEditLayerType Type)
{
	switch (Type)
	{
	case EEditLayerType::TerrainModify:    return TEXT("terrain_modify");
	case EEditLayerType::BuildingAdd:      return TEXT("building_add");
	case EEditLayerType::BuildingRemove:   return TEXT("building_remove");
	case EEditLayerType::BuildingHeight:   return TEXT("building_height");
	case EEditLayerType::RoadBlock:        return TEXT("road_block");
	case EEditLayerType::RoadAdd:          return TEXT("road_add");
	case EEditLayerType::RoadWidth:        return TEXT("road_width");
	case EEditLayerType::DestructionState: return TEXT("destruction_state");
	case EEditLayerType::Marker:           return TEXT("marker");
	case EEditLayerType::BuildingAddBatch: return TEXT("building_add_batch");
	default:                               return TEXT("marker");
	}
}

EEditLayerType UEditLayerManager::StringToLayerType(const FString& Str)
{
	if (Str == TEXT("terrain_modify"))     return EEditLayerType::TerrainModify;
	if (Str == TEXT("building_add"))       return EEditLayerType::BuildingAdd;
	if (Str == TEXT("building_remove"))    return EEditLayerType::BuildingRemove;
	if (Str == TEXT("building_height"))    return EEditLayerType::BuildingHeight;
	if (Str == TEXT("road_block"))         return EEditLayerType::RoadBlock;
	if (Str == TEXT("road_add"))           return EEditLayerType::RoadAdd;
	if (Str == TEXT("road_width"))         return EEditLayerType::RoadWidth;
	if (Str == TEXT("destruction_state"))  return EEditLayerType::DestructionState;
	if (Str == TEXT("marker"))             return EEditLayerType::Marker;
	if (Str == TEXT("building_add_batch")) return EEditLayerType::BuildingAddBatch;
	return EEditLayerType::Marker;
}

FString UEditLayerManager::AreaTypeToString(EAreaType Type)
{
	switch (Type)
	{
	case EAreaType::Polygon:  return TEXT("polygon");
	case EAreaType::Circle:   return TEXT("circle");
	case EAreaType::Point:    return TEXT("point");
	case EAreaType::Path:     return TEXT("path");
	case EAreaType::ActorRef: return TEXT("actor_ref");
	default:                  return TEXT("point");
	}
}

EAreaType UEditLayerManager::StringToAreaType(const FString& Str)
{
	if (Str == TEXT("polygon"))   return EAreaType::Polygon;
	if (Str == TEXT("circle"))    return EAreaType::Circle;
	if (Str == TEXT("point"))     return EAreaType::Point;
	if (Str == TEXT("path"))      return EAreaType::Path;
	if (Str == TEXT("actor_ref")) return EAreaType::ActorRef;
	return EAreaType::Point;
}

FString UEditLayerManager::CreatedByToString(ELayerCreatedBy CreatedBy)
{
	switch (CreatedBy)
	{
	case ELayerCreatedBy::Slider:          return TEXT("slider");
	case ELayerCreatedBy::Preset:          return TEXT("preset");
	case ELayerCreatedBy::ReferencePreset: return TEXT("reference_preset");
	case ELayerCreatedBy::AiSuggest:       return TEXT("ai_suggest");
	case ELayerCreatedBy::Manual:          return TEXT("manual");
	default:                               return TEXT("manual");
	}
}

ELayerCreatedBy UEditLayerManager::StringToCreatedBy(const FString& Str)
{
	if (Str == TEXT("slider"))           return ELayerCreatedBy::Slider;
	if (Str == TEXT("preset"))           return ELayerCreatedBy::Preset;
	if (Str == TEXT("reference_preset")) return ELayerCreatedBy::ReferencePreset;
	if (Str == TEXT("ai_suggest"))       return ELayerCreatedBy::AiSuggest;
	if (Str == TEXT("manual"))           return ELayerCreatedBy::Manual;
	return ELayerCreatedBy::Manual;
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON helpers — FVector2D array ↔ [[x,y], ...]
// ─────────────────────────────────────────────────────────────────────────────

static TArray<TSharedPtr<FJsonValue>> Vector2DArrayToJsonValues(const TArray<FVector2D>& Points)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Points.Num());
	for (const FVector2D& Pt : Points)
	{
		TArray<TSharedPtr<FJsonValue>> Pair;
		Pair.Add(MakeShared<FJsonValueNumber>(Pt.X));
		Pair.Add(MakeShared<FJsonValueNumber>(Pt.Y));
		Arr.Add(MakeShared<FJsonValueArray>(Pair));
	}
	return Arr;
}

static bool JsonValuesToVector2DArray(const TArray<TSharedPtr<FJsonValue>>& Arr, TArray<FVector2D>& Out)
{
	Out.Empty(Arr.Num());
	for (const auto& Val : Arr)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pair;
		if (!Val->TryGetArray(Pair) || Pair->Num() < 2) return false;
		Out.Add(FVector2D((*Pair)[0]->AsNumber(), (*Pair)[1]->AsNumber()));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON serialization — Area
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> UEditLayerManager::AreaToJson(const FEditLayerArea& Area) const
{
	auto Obj  = MakeShared<FJsonObject>();
	auto Data = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("type"), AreaTypeToString(Area.Type));

	switch (Area.Type)
	{
	case EAreaType::Polygon:
	{
		Data->SetField(TEXT("points_ue5"),
			MakeShared<FJsonValueArray>(Vector2DArrayToJsonValues(Area.Polygon.PointsUE5)));
		break;
	}
	case EAreaType::Circle:
	{
		TArray<TSharedPtr<FJsonValue>> Center;
		Center.Add(MakeShared<FJsonValueNumber>(Area.Circle.CenterUE5.X));
		Center.Add(MakeShared<FJsonValueNumber>(Area.Circle.CenterUE5.Y));
		Data->SetField(TEXT("center_ue5"), MakeShared<FJsonValueArray>(Center));
		Data->SetNumberField(TEXT("radius_cm"), Area.Circle.RadiusCm);
		break;
	}
	case EAreaType::Point:
	{
		TArray<TSharedPtr<FJsonValue>> Loc;
		Loc.Add(MakeShared<FJsonValueNumber>(Area.Point.LocationUE5.X));
		Loc.Add(MakeShared<FJsonValueNumber>(Area.Point.LocationUE5.Y));
		Loc.Add(MakeShared<FJsonValueNumber>(Area.Point.LocationUE5.Z));
		Data->SetField(TEXT("location_ue5"), MakeShared<FJsonValueArray>(Loc));
		break;
	}
	case EAreaType::Path:
	{
		Data->SetField(TEXT("points_ue5"),
			MakeShared<FJsonValueArray>(Vector2DArrayToJsonValues(Area.Path.PointsUE5)));
		Data->SetNumberField(TEXT("width_cm"), Area.Path.WidthCm);
		break;
	}
	case EAreaType::ActorRef:
	{
		Data->SetStringField(TEXT("stable_id"), Area.ActorRef.StableId);
		TArray<TSharedPtr<FJsonValue>> Fb;
		Fb.Add(MakeShared<FJsonValueNumber>(Area.ActorRef.FallbackLocationUE5.X));
		Fb.Add(MakeShared<FJsonValueNumber>(Area.ActorRef.FallbackLocationUE5.Y));
		Fb.Add(MakeShared<FJsonValueNumber>(Area.ActorRef.FallbackLocationUE5.Z));
		Data->SetField(TEXT("fallback_location_ue5"), MakeShared<FJsonValueArray>(Fb));
		break;
	}
	}

	Obj->SetObjectField(TEXT("data"), Data);
	return Obj;
}

bool UEditLayerManager::JsonToArea(
	const TSharedPtr<FJsonObject>& JsonObj, FEditLayerArea& OutArea) const
{
	if (!JsonObj) return false;
	OutArea.Type = StringToAreaType(JsonObj->GetStringField(TEXT("type")));

	const TSharedPtr<FJsonObject>* DataPtr;
	if (!JsonObj->TryGetObjectField(TEXT("data"), DataPtr)) return false;
	const auto& Data = *DataPtr;

	switch (OutArea.Type)
	{
	case EAreaType::Polygon:
	{
		const TArray<TSharedPtr<FJsonValue>>* Pts;
		if (Data->TryGetArrayField(TEXT("points_ue5"), Pts))
			JsonValuesToVector2DArray(*Pts, OutArea.Polygon.PointsUE5);
		break;
	}
	case EAreaType::Circle:
	{
		const TArray<TSharedPtr<FJsonValue>>* Ctr;
		if (Data->TryGetArrayField(TEXT("center_ue5"), Ctr) && Ctr->Num() >= 2)
			OutArea.Circle.CenterUE5 = FVector2D((*Ctr)[0]->AsNumber(), (*Ctr)[1]->AsNumber());
		OutArea.Circle.RadiusCm = Data->GetNumberField(TEXT("radius_cm"));
		break;
	}
	case EAreaType::Point:
	{
		const TArray<TSharedPtr<FJsonValue>>* Loc;
		if (Data->TryGetArrayField(TEXT("location_ue5"), Loc) && Loc->Num() >= 3)
			OutArea.Point.LocationUE5 = FVector(
				(*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber());
		break;
	}
	case EAreaType::Path:
	{
		const TArray<TSharedPtr<FJsonValue>>* Pts;
		if (Data->TryGetArrayField(TEXT("points_ue5"), Pts))
			JsonValuesToVector2DArray(*Pts, OutArea.Path.PointsUE5);
		OutArea.Path.WidthCm = Data->GetNumberField(TEXT("width_cm"));
		break;
	}
	case EAreaType::ActorRef:
	{
		OutArea.ActorRef.StableId = Data->GetStringField(TEXT("stable_id"));
		const TArray<TSharedPtr<FJsonValue>>* Fb;
		if (Data->TryGetArrayField(TEXT("fallback_location_ue5"), Fb) && Fb->Num() >= 3)
			OutArea.ActorRef.FallbackLocationUE5 = FVector(
				(*Fb)[0]->AsNumber(), (*Fb)[1]->AsNumber(), (*Fb)[2]->AsNumber());
		break;
	}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON serialization — Layer
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> UEditLayerManager::LayerToJson(const FEditLayer& Layer) const
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("layer_id"),   Layer.LayerId);
	Obj->SetStringField(TEXT("type"),       LayerTypeToString(Layer.Type));
	Obj->SetObjectField(TEXT("area"),       AreaToJson(Layer.Area));
	Obj->SetObjectField(TEXT("params"),     Layer.Params.IsValid()
		? Layer.Params : MakeShared<FJsonObject>());
	Obj->SetStringField(TEXT("label"),      Layer.Label);
	Obj->SetBoolField  (TEXT("locked"),     Layer.bLocked);
	Obj->SetBoolField  (TEXT("visible"),    Layer.bVisible);
	Obj->SetStringField(TEXT("created_by"), CreatedByToString(Layer.CreatedBy));

	if (!Layer.SourceId.IsEmpty())
	{
		Obj->SetStringField(TEXT("source_id"),  Layer.SourceId);
		Obj->SetNumberField(TEXT("generation"), Layer.Generation);
	}

	return Obj;
}

bool UEditLayerManager::JsonToLayer(
	const TSharedPtr<FJsonObject>& JsonObj, FEditLayer& OutLayer) const
{
	if (!JsonObj) return false;

	OutLayer.LayerId   = JsonObj->GetStringField(TEXT("layer_id"));
	OutLayer.Type      = StringToLayerType(JsonObj->GetStringField(TEXT("type")));
	OutLayer.Label     = JsonObj->GetStringField(TEXT("label"));
	OutLayer.bLocked   = JsonObj->GetBoolField(TEXT("locked"));
	OutLayer.bVisible  = JsonObj->GetBoolField(TEXT("visible"));
	OutLayer.CreatedBy = StringToCreatedBy(JsonObj->GetStringField(TEXT("created_by")));

	const TSharedPtr<FJsonObject>* AreaObj;
	if (JsonObj->TryGetObjectField(TEXT("area"), AreaObj))
	{
		JsonToArea(*AreaObj, OutLayer.Area);
	}

	const TSharedPtr<FJsonObject>* ParamsObj;
	if (JsonObj->TryGetObjectField(TEXT("params"), ParamsObj))
	{
		OutLayer.Params = *ParamsObj;
	}

	OutLayer.SourceId   = JsonObj->GetStringField(TEXT("source_id"));
	OutLayer.Generation = static_cast<int32>(JsonObj->GetNumberField(TEXT("generation")));
	if (OutLayer.Generation < 1) OutLayer.Generation = 1;

	return !OutLayer.LayerId.IsEmpty();
}

// ─────────────────────────────────────────────────────────────────────────────
//  배치 ↔ 개별 전환
// ─────────────────────────────────────────────────────────────────────────────

int32 UEditLayerManager::ExplodeBatchLayer(const FString& BatchLayerId)
{
	FEditLayer* Batch = FindLayer(BatchLayerId);
	if (!Batch || Batch->Type != EEditLayerType::BuildingAddBatch) return 0;
	if (!Batch->Params.IsValid()) return 0;

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Batch->Params->TryGetArrayField(TEXT("batch_items"), Items)) return 0;

	int32 Count = 0;
	for (int32 i = 0; i < Items->Num(); ++i)
	{
		const auto& Item = (*Items)[i]->AsObject();
		if (!Item) continue;

		FEditLayer Sub;
		Sub.Type = EEditLayerType::BuildingAdd;
		Sub.CreatedBy = Batch->CreatedBy;
		Sub.SourceId  = Batch->SourceId;
		Sub.Label = FString::Printf(TEXT("%s [%d]"), *Batch->Label, i + 1);

		const TArray<TSharedPtr<FJsonValue>>& Loc = Item->GetArrayField(TEXT("location_ue5"));
		if (Loc.Num() >= 3)
		{
			Sub.Area.Type = EAreaType::Point;
			Sub.Area.Point.LocationUE5 = FVector(
				Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
		}

		Sub.Params = MakeShared<FJsonObject>();
		Sub.Params->SetStringField(TEXT("type"), Item->GetStringField(TEXT("type_key")));
		Sub.Params->SetNumberField(TEXT("height_m"), Item->GetNumberField(TEXT("height_m")));
		Sub.Params->SetNumberField(TEXT("rotation_deg"), Item->GetNumberField(TEXT("rotation_deg")));

		AddLayer(MoveTemp(Sub));
		++Count;
	}

	RemoveLayer(BatchLayerId);
	return Count;
}

FString UEditLayerManager::MergeToBatchLayer(
	const TArray<FString>& LayerIdsToMerge, const FString& Label)
{
	TArray<TSharedPtr<FJsonValue>> Items;

	for (const FString& Id : LayerIdsToMerge)
	{
		const FEditLayer* L = FindLayer(Id);
		if (!L || L->Type != EEditLayerType::BuildingAdd) continue;

		auto Item = MakeShared<FJsonObject>();
		Item->SetArrayField(TEXT("location_ue5"),
			TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(L->Area.Point.LocationUE5.X),
				MakeShared<FJsonValueNumber>(L->Area.Point.LocationUE5.Y),
				MakeShared<FJsonValueNumber>(L->Area.Point.LocationUE5.Z)
			});
		if (L->Params.IsValid())
		{
			Item->SetStringField(TEXT("type_key"), L->Params->GetStringField(TEXT("type")));
			Item->SetNumberField(TEXT("height_m"), L->Params->GetNumberField(TEXT("height_m")));
			Item->SetNumberField(TEXT("rotation_deg"),
				L->Params->HasField(TEXT("rotation_deg"))
				? L->Params->GetNumberField(TEXT("rotation_deg")) : 0.0);
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	if (Items.Num() == 0) return FString();

	FEditLayer Batch;
	Batch.Type = EEditLayerType::BuildingAddBatch;
	Batch.CreatedBy = ELayerCreatedBy::Manual;
	Batch.Label = Label.IsEmpty() ? TEXT("일괄 건물 추가") : Label;
	Batch.Params = MakeShared<FJsonObject>();
	Batch.Params->SetArrayField(TEXT("batch_items"), Items);

	FString BatchId = AddLayer(MoveTemp(Batch));

	for (const FString& Id : LayerIdsToMerge)
		RemoveLayer(Id);

	return BatchId;
}

// ─────────────────────────────────────────────────────────────────────────────
//  교차 레이어 충돌 감지
// ─────────────────────────────────────────────────────────────────────────────

TArray<UEditLayerManager::FLayerConflict> UEditLayerManager::DetectLayerConflicts() const
{
	TArray<FLayerConflict> Results;

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FEditLayer& A = Layers[i];
		if (!A.bVisible) continue;

		for (int32 j = i + 1; j < Layers.Num(); ++j)
		{
			const FEditLayer& B = Layers[j];
			if (!B.bVisible) continue;

			if (A.Area.Type == EAreaType::ActorRef && B.Area.Type == EAreaType::ActorRef
				&& A.Area.ActorRef.StableId == B.Area.ActorRef.StableId)
			{
				FLayerConflict C;
				C.LayerIdA = A.LayerId;
				C.LayerIdB = B.LayerId;
				C.Reason = FString::Printf(TEXT("동일 Actor(%s) 중복 수정"), *A.Area.ActorRef.StableId);
				Results.Add(MoveTemp(C));
			}

			if (A.Type == EEditLayerType::BuildingAdd && B.Type == EEditLayerType::BuildingAdd
				&& A.Area.Type == EAreaType::Point && B.Area.Type == EAreaType::Point)
			{
				float Dist = FVector::Dist(A.Area.Point.LocationUE5, B.Area.Point.LocationUE5);
				if (Dist < 200.f)
				{
					FLayerConflict C;
					C.LayerIdA = A.LayerId;
					C.LayerIdB = B.LayerId;
					C.Reason = FString::Printf(TEXT("건물 추가 위치 근접 (%.0fcm)"), Dist);
					Results.Add(MoveTemp(C));
				}
			}
		}
	}

	return Results;
}

// ─────────────────────────────────────────────────────────────────────────────
//  File I/O — Save
// ─────────────────────────────────────────────────────────────────────────────

bool UEditLayerManager::SaveToJson() const
{
	if (MapId.IsEmpty())
	{
		UE_LOG(LogEditLayer, Warning, TEXT("Cannot save: MapId is empty"));
		return false;
	}

	auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("version"), kLayersJsonVersion);
	Root->SetStringField(TEXT("map_id"),  MapId);

	TArray<TSharedPtr<FJsonValue>> LayersArr;
	LayersArr.Reserve(Layers.Num());
	for (const FEditLayer& L : Layers)
	{
		LayersArr.Add(MakeShared<FJsonValueObject>(LayerToJson(L)));
	}
	Root->SetArrayField(TEXT("layers"), LayersArr);

	FString JsonStr;
	auto Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogEditLayer, Error, TEXT("JSON serialization failed"));
		return false;
	}

	const FString FilePath = GetLayersJsonPath();
	const FString Dir = FPaths::GetPath(FilePath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir))
	{
		PF.CreateDirectoryTree(*Dir);
	}

	if (!FFileHelper::SaveStringToFile(JsonStr, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogEditLayer, Error, TEXT("Failed to write %s"), *FilePath);
		return false;
	}

	UE_LOG(LogEditLayer, Log, TEXT("Saved %d layers → %s"), Layers.Num(), *FilePath);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  File I/O — Load
// ─────────────────────────────────────────────────────────────────────────────

bool UEditLayerManager::LoadFromJson()
{
	const FString FilePath = GetLayersJsonPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
	{
		return true;   // file not yet created — fresh map
	}

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogEditLayer, Error, TEXT("Failed to parse %s"), *FilePath);
		return false;
	}

	const FString Version = Root->GetStringField(TEXT("version"));
	if (Version != kLayersJsonVersion)
	{
		UE_LOG(LogEditLayer, Warning,
			TEXT("Version mismatch: expected %s, got %s"), kLayersJsonVersion, *Version);
	}

	const TArray<TSharedPtr<FJsonValue>>* LayersArr;
	if (!Root->TryGetArrayField(TEXT("layers"), LayersArr))
	{
		UE_LOG(LogEditLayer, Warning, TEXT("No 'layers' array in %s"), *FilePath);
		return true;
	}

	Layers.Empty(LayersArr->Num());
	for (const auto& Val : *LayersArr)
	{
		const TSharedPtr<FJsonObject>* LayerObj;
		if (!Val->TryGetObject(LayerObj)) continue;

		FEditLayer Layer;
		if (JsonToLayer(*LayerObj, Layer))
		{
			Layers.Emplace(MoveTemp(Layer));
		}
	}

	UE_LOG(LogEditLayer, Log, TEXT("Loaded %d layers from %s"), Layers.Num(), *FilePath);
	return true;
}
