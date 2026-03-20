#include "DesignerIntentSubsystem.h"
#include "LevelToolSubsystem.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"
#include "SliderLayerGenerator.h"
#include "PresetManager.h"
#include "ChecklistEngine.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogDesignerIntent, Log, All);

const FSliderState UDesignerIntentSubsystem::DefaultSliderState = {};

// 8방향 시선 분석용 단위 벡터 (45° 간격)
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
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	SliderStates.Add(ESliderType::UrbanDensity,      FSliderState());
	SliderStates.Add(ESliderType::Openness,           FSliderState());
	SliderStates.Add(ESliderType::RouteComplexity,    FSliderState());
	SliderStates.Add(ESliderType::ElevationContrast,  FSliderState());
	SliderStates.Add(ESliderType::DestructionLevel,   FSliderState());

	LayerGenerator = NewObject<USliderLayerGenerator>(this);
	LayerGenerator->Initialize(this);

	PresetMgr = NewObject<UPresetManager>(this);
	PresetMgr->Initialize(this);

	CheckEngine = NewObject<UChecklistEngine>(this);
	CheckEngine->Initialize(this);

	TryRestoreSession();

	UE_LOG(LogDesignerIntent, Log, TEXT("DesignerIntent subsystem initialized"));
}

void UDesignerIntentSubsystem::Deinitialize()
{
	CachedBuildings.Empty();
	CachedRoads.Empty();
	Super::Deinitialize();
}

void UDesignerIntentSubsystem::TryRestoreSession()
{
	UEditLayerManager* LM = GetLayerManager();
	if (!LM) return;

	const FString LayersPath = LM->GetLayersJsonPath();
	if (!FPaths::FileExists(LayersPath)) return;

	if (!LM->LoadFromJson())
	{
		UE_LOG(LogDesignerIntent, Warning, TEXT("세션 복원 실패: layers.json 로드 에러"));
		return;
	}

	int32 VisibleCount = 0;
	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (L.bVisible) ++VisibleCount;
	}

	UEditLayerApplicator* Applicator = LM->GetApplicator();
	if (Applicator)
	{
		Applicator->LoadTerrainBaseFromDisk();
	}

	const FString MetaPath = GetMapMetaPath();
	if (FPaths::FileExists(MetaPath))
	{
		LoadMapMetaData(MetaPath);
		LoadPredictionHistoryFromMeta();
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("세션 복원: %d 레이어 (%d visible)"),
		LM->GetAllLayers().Num(), VisibleCount);
}

// ─────────────────────────────────────────────────────────────────────────────
//  1단계 연동
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::OnStage1Complete(const FString& MapId)
{
	ActiveMapId = MapId;

	const FString MetaPath = GetMapMetaPath();
	if (!LoadMapMetaData(MetaPath))
	{
		UE_LOG(LogDesignerIntent, Warning,
			TEXT("map_meta.json 로드 실패 — 슬라이더 초기값 산출을 건너뜁니다"));
		return;
	}

	auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>();
	if (LTS)
	{
		const FLevelToolJobResult& R = LTS->GetLastResult();
		LoadBuildingData(R.BuildingsJsonPath);
		LoadRoadData(R.RoadsJsonPath);
		LoadWaterData(R.WaterJsonPath);

		if (R.ElevationMinM != 0.f || R.ElevationMaxM != 0.f)
		{
			ElevationMinM = R.ElevationMinM;
			ElevationMaxM = R.ElevationMaxM;
		}
	}

	ComputeSliderInitialValues();
	ComputeAchievementLimits();
	SaveSliderValuesToMapMeta();

	UE_LOG(LogDesignerIntent, Log,
		TEXT("슬라이더 초기값 산출 완료 — Density=%.0f  Open=%.0f  Route=%.0f  Elev=%.0f  Destruct=%.0f"),
		SliderStates[ESliderType::UrbanDensity].InitialValue,
		SliderStates[ESliderType::Openness].InitialValue,
		SliderStates[ESliderType::RouteComplexity].InitialValue,
		SliderStates[ESliderType::ElevationContrast].InitialValue,
		SliderStates[ESliderType::DestructionLevel].InitialValue);

	OnSliderInitialized.Broadcast(MapId);
}

bool UDesignerIntentSubsystem::IsStage1Ready() const
{
	return !ActiveMapId.IsEmpty() && CachedBuildings.Num() > 0;
}

bool UDesignerIntentSubsystem::CheckStage1Prerequisites(TArray<FString>& OutMissing) const
{
	OutMissing.Empty();

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutMissing.Add(TEXT("에디터 월드가 열려있지 않습니다"));
		return false;
	}

	bool bHasLandscape = false;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		bHasLandscape = true;
		break;
	}
	if (!bHasLandscape)
		OutMissing.Add(TEXT("Landscape 액터가 월드에 없습니다"));

	auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>();
	if (LTS)
	{
		const FLevelToolJobResult& R = LTS->GetLastResult();
		if (R.BuildingsJsonPath.IsEmpty() || !FPaths::FileExists(R.BuildingsJsonPath))
			OutMissing.Add(TEXT("buildings.json 파일이 없습니다"));
	}
	else
	{
		OutMissing.Add(TEXT("LevelToolSubsystem을 사용할 수 없습니다"));
	}

	if (GetMapMetaPath().IsEmpty() || !FPaths::FileExists(GetMapMetaPath()))
		OutMissing.Add(TEXT("map_meta.json 파일이 없습니다"));

	return OutMissing.Num() == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  슬라이더 접근자
// ─────────────────────────────────────────────────────────────────────────────

float UDesignerIntentSubsystem::GetSliderInitialValue(ESliderType Type) const
{
	const FSliderState* S = SliderStates.Find(Type);
	return S ? S->InitialValue : 0.f;
}

const FSliderState& UDesignerIntentSubsystem::GetSliderState(ESliderType Type) const
{
	const FSliderState* S = SliderStates.Find(Type);
	return S ? *S : DefaultSliderState;
}

float UDesignerIntentSubsystem::GetSliderAchievementRate(ESliderType Type) const
{
	const FSliderState* S = SliderStates.Find(Type);
	return S ? S->AchievementRate : 100.f;
}

FString UDesignerIntentSubsystem::GetAchievementLimitReason(ESliderType Type) const
{
	const FSliderState* S = SliderStates.Find(Type);
	return S ? S->LimitReason : FString();
}

void UDesignerIntentSubsystem::ApplySlider(ESliderType Type, float NewValue)
{
	FSliderState* State = SliderStates.Find(Type);
	if (!State)
	{
		UE_LOG(LogDesignerIntent, Warning, TEXT("ApplySlider: 알 수 없는 슬라이더 타입"));
		return;
	}

	UEditLayerManager* LM = GetLayerManager();
	if (!LM || !LayerGenerator)
	{
		UE_LOG(LogDesignerIntent, Warning, TEXT("ApplySlider: LayerManager/Generator 없음"));
		return;
	}

	const float OldValue = State->CurrentValue;
	NewValue = FMath::Clamp(NewValue, 0.f, 100.f);

	TArray<FEditLayer> NewLayers = LayerGenerator->GenerateLayers(Type, OldValue, NewValue);

	const FString SourceId = USliderLayerGenerator::MakeSourceId(Type);

	// 기존 레이어의 Actor 먼저 정리
	if (UEditLayerApplicator* Applicator = LM->GetApplicator())
	{
		TArray<FEditLayer*> OldLayers = LM->FindLayersBySourceId(SourceId);
		for (FEditLayer* Old : OldLayers)
		{
			if (Old->bVisible)
			{
				Applicator->HideLayer(Old->LayerId);
			}
		}
	}

	// 데이터 교체 (ReplaceLayersBySourceId가 기존 삭제 + 새 추가)
	LM->ReplaceLayersBySourceId(SourceId, MoveTemp(NewLayers));

	// 새 레이어들을 Actor에 반영
	if (UEditLayerApplicator* Applicator = LM->GetApplicator())
	{
		TArray<FEditLayer*> Applied = LM->FindLayersBySourceId(SourceId);
		for (FEditLayer* L : Applied)
		{
			Applicator->ApplyLayer(L->LayerId);
		}
	}

	State->CurrentValue = NewValue;
	if (!bBatchApply)
	{
		LM->SaveToJson();
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("ApplySlider %d: %.1f → %.1f (%d 레이어)"),
		(int32)Type, OldValue, NewValue,
		LM->FindLayersBySourceId(SourceId).Num());
}

void UDesignerIntentSubsystem::EndBatchApply()
{
	bBatchApply = false;
	if (UEditLayerManager* LM = GetLayerManager())
	{
		LM->SaveToJson();
	}
}

UEditLayerManager* UDesignerIntentSubsystem::GetLayerManager() const
{
	auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>();
	return LTS ? LTS->GetEditLayerManager() : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  P1-8: 개별 Actor 단건 편집 — 우클릭 메뉴 등에서 호출
// ─────────────────────────────────────────────────────────────────────────────

bool UDesignerIntentSubsystem::SingleEditActor(AActor* Actor, ESingleEditType EditType, float Param)
{
	if (!Actor) return false;

	UEditLayerManager* LM = GetLayerManager();
	if (!LM) return false;

	FString StableId;
	for (const FName& Tag : Actor->Tags)
	{
		FString TagStr = Tag.ToString();
		if (TagStr.StartsWith(TEXT("bldg_")) || TagStr.StartsWith(TEXT("road_")))
		{
			StableId = TagStr;
			break;
		}
	}
	if (StableId.IsEmpty())
	{
		StableId = FString::Printf(TEXT("bldg_manual_%s"), *FGuid::NewGuid().ToString().Left(8));
	}

	const FVector Loc = Actor->GetActorLocation();
	FEditLayer Layer;
	Layer.CreatedBy = ELayerCreatedBy::Manual;

	switch (EditType)
	{
	case ESingleEditType::Remove:
		Layer.Type  = EEditLayerType::BuildingRemove;
		Layer.Label = FString::Printf(TEXT("[수동] 건물 제거 %s"), *Actor->GetActorLabel());
		Layer.Area.Type = EAreaType::ActorRef;
		Layer.Area.ActorRef.StableId = StableId;
		Layer.Area.ActorRef.FallbackLocationUE5 = Loc;
		Layer.Params = MakeShared<FJsonObject>();
		Layer.Params->SetBoolField(TEXT("keep_foundation"), false);
		break;

	case ESingleEditType::HeightChange:
		Layer.Type  = EEditLayerType::BuildingHeight;
		Layer.Label = FString::Printf(TEXT("[수동] 높이 변경 %s (×%.1f)"), *Actor->GetActorLabel(), Param);
		Layer.Area.Type = EAreaType::ActorRef;
		Layer.Area.ActorRef.StableId = StableId;
		Layer.Area.ActorRef.FallbackLocationUE5 = Loc;
		Layer.Params = MakeShared<FJsonObject>();
		Layer.Params->SetNumberField(TEXT("scale_factor"), Param > 0.f ? Param : 1.5f);
		break;

	case ESingleEditType::DestructionChange:
	{
		Layer.Type  = EEditLayerType::DestructionState;
		FString State = (Param <= 0.5f) ? TEXT("partial") : TEXT("destroyed");
		Layer.Label = FString::Printf(TEXT("[수동] 파괴 %s → %s"), *Actor->GetActorLabel(), *State);
		Layer.Area.Type = EAreaType::ActorRef;
		Layer.Area.ActorRef.StableId = StableId;
		Layer.Area.ActorRef.FallbackLocationUE5 = Loc;
		Layer.Params = MakeShared<FJsonObject>();
		Layer.Params->SetStringField(TEXT("new_state"), State);
		break;
	}

	case ESingleEditType::AddMarker:
		Layer.Type  = EEditLayerType::Marker;
		Layer.Label = FString::Printf(TEXT("[수동] 마커 %s"), *Actor->GetActorLabel());
		Layer.Area.Type = EAreaType::Point;
		Layer.Area.Point.LocationUE5 = Loc;
		Layer.Params = MakeShared<FJsonObject>();
		Layer.Params->SetStringField(TEXT("marker_type"), TEXT("custom"));
		break;

	default:
		return false;
	}

	FEditLayer LayerCopy = Layer;
	const FString LayerId = LayerCopy.LayerId.IsEmpty()
		? LM->AddLayer(MoveTemp(LayerCopy))
		: (LM->AddLayer(MoveTemp(LayerCopy)), Layer.LayerId);

	if (UEditLayerApplicator* App = LM->GetApplicator())
		App->ApplyLayer(LayerId);

	LM->SaveToJson();

	UE_LOG(LogDesignerIntent, Log, TEXT("SingleEditActor: %s → %d 레이어 생성"),
		*Actor->GetActorLabel(), (int32)EditType);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  P1-10: 맵 규모별 파라미터 스케일링 — 기준 반경 1.5km
// ─────────────────────────────────────────────────────────────────────────────

float UDesignerIntentSubsystem::GetScaledDBSCAN_Eps() const
{
	const float BaseEps = 15000.f; // 150m for 1.5km radius
	const float Scale   = FMath::Max(0.5f, MapRadiusKm / 1.5f);
	return BaseEps * Scale;
}

float UDesignerIntentSubsystem::GetScaledGridCellCm() const
{
	const float BaseCell = 5000.f; // 50m for 1.5km radius
	const float Scale    = FMath::Max(0.5f, MapRadiusKm / 1.5f);
	return BaseCell * Scale;
}

float UDesignerIntentSubsystem::GetScaledMinPoiSpacing() const
{
	const float RadCm = MapRadiusKm * 100000.f;
	return RadCm * 0.2f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  P2-20: 예측 오차 보정 — EMA (Exponential Moving Average)
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::RecordPredictionSample(float Predicted, float Actual)
{
	const float Alpha = 0.3f;
	float Error = Actual - Predicted;
	PredictionBias  = PredictionBias  * (1.f - Alpha) + Error * Alpha;
	PredictionScale = FMath::Clamp(PredictionScale + (Error > 0 ? 0.01f : -0.01f), 0.5f, 2.0f);
	PredictionHistory.Add(FPredictionSample{ Predicted, Actual, Error, FDateTime::UtcNow() });
	UE_LOG(LogDesignerIntent, Verbose, TEXT("예측 보정: bias=%.2f scale=%.2f (err=%.2f)"),
		PredictionBias, PredictionScale, Error);

	SavePredictionHistoryToMeta();
}

// ─────────────────────────────────────────────────────────────────────────────
//  G-7: EffectiveState 재구성 — 모든 Visible 레이어를 순회하여 분석 캐시 갱신
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::RebuildEffectiveState()
{
	UEditLayerManager* LM = GetLayerManager();
	if (!LM) return;

	TSet<FString> RemovedBldgIds;
	TMap<FString, float> HeightScales;
	TArray<FBuildingAnalysis> AddedBuildings;
	TArray<FRoadAnalysis> AddedRoads;
	TSet<FString> BlockedRoadIds;

	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (!L.bVisible) continue;

		switch (L.Type)
		{
		case EEditLayerType::BuildingRemove:
			RemovedBldgIds.Add(L.Area.ActorRef.StableId);
			break;
		case EEditLayerType::BuildingHeight:
		{
			float Scale = 1.5f;
			if (L.Params.IsValid() && L.Params->HasField(TEXT("scale_factor")))
				Scale = L.Params->GetNumberField(TEXT("scale_factor"));
			HeightScales.FindOrAdd(L.Area.ActorRef.StableId) = Scale;
			break;
		}
		case EEditLayerType::BuildingAdd:
		{
			FBuildingAnalysis BA;
			BA.OsmId = 0;
			BA.CentroidUE5 = FVector2D(L.Area.Point.LocationUE5.X, L.Area.Point.LocationUE5.Y);
			BA.HeightM = 10.f;
			BA.AreaM2  = 100.f;
			if (L.Params.IsValid())
			{
				BA.HeightM = L.Params->GetNumberField(TEXT("height_m"));
				BA.AreaM2  = L.Params->GetNumberField(TEXT("footprint_m2"));
				BA.TypeKey = L.Params->GetStringField(TEXT("type_key"));
			}
			AddedBuildings.Add(MoveTemp(BA));
			break;
		}
		case EEditLayerType::RoadAdd:
		{
			FRoadAnalysis RA;
			RA.OsmId = 0;
			RA.RoadType = TEXT("added");
			RA.PointsUE5 = L.Area.Path.PointsUE5;
			AddedRoads.Add(MoveTemp(RA));
			break;
		}
		case EEditLayerType::RoadBlock:
			BlockedRoadIds.Add(L.Area.ActorRef.StableId);
			break;
		default:
			break;
		}
	}

	EffectiveBuildings.Empty();
	for (const FBuildingAnalysis& B : CachedBuildings)
	{
		FString BId = FString::Printf(TEXT("bldg_%lld"), B.OsmId);
		if (RemovedBldgIds.Contains(BId)) continue;

		FBuildingAnalysis Copy = B;
		if (float* Scale = HeightScales.Find(BId))
			Copy.HeightM *= *Scale;
		EffectiveBuildings.Add(MoveTemp(Copy));
	}
	EffectiveBuildings.Append(AddedBuildings);

	EffectiveRoads.Empty();
	for (const FRoadAnalysis& R : CachedRoads)
	{
		FString RId = FString::Printf(TEXT("road_%lld"), R.OsmId);
		if (BlockedRoadIds.Contains(RId)) continue;
		EffectiveRoads.Add(R);
	}
	EffectiveRoads.Append(AddedRoads);

	UE_LOG(LogDesignerIntent, Log,
		TEXT("EffectiveState: 건물 %d (원본 %d), 도로 %d (원본 %d)"),
		EffectiveBuildings.Num(), CachedBuildings.Num(),
		EffectiveRoads.Num(), CachedRoads.Num());
}

// ─────────────────────────────────────────────────────────────────────────────
//  FEffectiveRoadGraph 구현
// ─────────────────────────────────────────────────────────────────────────────

void FEffectiveRoadGraph::BuildFromRoads(const TArray<TArray<FVector2D>>& RoadPolylines, float CellSizeCm)
{
	Nodes.Empty();
	Edges.Empty();

	auto PointToKey = [CellSizeCm](const FVector2D& P) -> int64
	{
		int32 GX = FMath::FloorToInt(P.X / CellSizeCm);
		int32 GY = FMath::FloorToInt(P.Y / CellSizeCm);
		return ((int64)GX << 32) | (int64)(uint32)GY;
	};

	for (const TArray<FVector2D>& Polyline : RoadPolylines)
	{
		int64 PrevKey = 0;
		bool bFirst = true;

		for (const FVector2D& Pt : Polyline)
		{
			int64 Key = PointToKey(Pt);
			FRoadGraphNode& N = Nodes.FindOrAdd(Key);
			if (N.CellKey == 0)
			{
				N.CellKey  = Key;
				N.Location = Pt;
			}

			if (!bFirst && Key != PrevKey)
			{
				N.Degree++;
				Nodes[PrevKey].Degree++;

				FRoadGraphEdge Edge;
				Edge.NodeA = FMath::Min(PrevKey, Key);
				Edge.NodeB = FMath::Max(PrevKey, Key);
				Edge.LengthCm = FVector2D::Distance(Nodes[PrevKey].Location, N.Location);
				Edges.Add(MoveTemp(Edge));
			}

			PrevKey = Key;
			bFirst = false;
		}
	}
}

TArray<int64> FEffectiveRoadGraph::FindBridgeEdgeNodePairs() const
{
	TArray<int64> Result;
	TMap<int64, TArray<int32>> AdjList;
	for (auto& KV : Nodes)
		AdjList.FindOrAdd(KV.Key);

	for (int32 i = 0; i < Edges.Num(); ++i)
	{
		AdjList.FindOrAdd(Edges[i].NodeA).Add(i);
		AdjList.FindOrAdd(Edges[i].NodeB).Add(i);
	}

	TMap<int64, int32> DiscTime;
	TMap<int64, int32> LowTime;
	int32 Timer = 0;

	TFunction<void(int64, int64)> DFS = [&](int64 U, int64 Parent)
	{
		DiscTime.Add(U, Timer);
		LowTime.Add(U, Timer);
		Timer++;

		for (int32 EIdx : AdjList[U])
		{
			int64 V = (Edges[EIdx].NodeA == U) ? Edges[EIdx].NodeB : Edges[EIdx].NodeA;
			if (!DiscTime.Contains(V))
			{
				DFS(V, U);
				LowTime[U] = FMath::Min(LowTime[U], LowTime[V]);
				if (LowTime[V] > DiscTime[U])
				{
					Result.Add(U);
					Result.Add(V);
				}
			}
			else if (V != Parent)
			{
				LowTime[U] = FMath::Min(LowTime[U], DiscTime[V]);
			}
		}
	};

	for (auto& KV : Nodes)
	{
		if (!DiscTime.Contains(KV.Key))
		{
			DFS(KV.Key, -1);
		}
	}

	return Result;
}

float FEffectiveRoadGraph::AverageNodeDegree() const
{
	if (Nodes.Num() == 0) return 0.f;
	float Sum = 0.f;
	for (auto& KV : Nodes) Sum += KV.Value.Degree;
	return Sum / Nodes.Num();
}

// ─────────────────────────────────────────────────────────────────────────────
//  슬라이더 초기값 산출
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::ComputeSliderInitialValues()
{
	auto Set = [this](ESliderType T, float V)
	{
		FSliderState& S = SliderStates.FindOrAdd(T);
		S.InitialValue  = V;
		S.CurrentValue  = V;
	};

	Set(ESliderType::UrbanDensity,     ComputeUrbanDensity());
	Set(ESliderType::Openness,         ComputeOpenness());
	Set(ESliderType::RouteComplexity,  ComputeRouteComplexity());
	Set(ESliderType::ElevationContrast,ComputeElevationContrast());
	Set(ESliderType::DestructionLevel, ComputeDestructionLevel());
}

// ── 도심 밀도: min(100, 건물수 / (맵면적_km² × 50) × 100) ─────────────────
float UDesignerIntentSubsystem::ComputeUrbanDensity() const
{
	if (CachedBuildings.Num() == 0 || MapRadiusKm <= 0.f) return 0.f;

	const float AreaKm2 = PI * MapRadiusKm * MapRadiusKm;
	const float Raw = static_cast<float>(CachedBuildings.Num()) / (AreaKm2 * 50.f) * 100.f;
	return FMath::Min(100.f, Raw);
}

// ── 개방도: 200m 그리드 8방향 시선 비차단 비율 ──────────────────────────────
float UDesignerIntentSubsystem::ComputeOpenness() const
{
	if (CachedBuildings.Num() == 0) return 100.f;
	if (MapRadiusKm <= 0.f) return 100.f;

	const float RadiusCm    = MapRadiusKm * 100000.f;
	const float GridCellCm  = 20000.f;        // 200m
	const float CheckRangeCm = 20000.f;       // 200m
	const float MinBlockHeightM = 3.f;

	const int32 GridHalf = FMath::CeilToInt(RadiusCm / GridCellCm);
	int32 TotalDirs    = 0;
	int32 OpenDirs     = 0;

	for (int32 gx = -GridHalf; gx <= GridHalf; ++gx)
	{
		for (int32 gy = -GridHalf; gy <= GridHalf; ++gy)
		{
			const FVector2D CellCenter(gx * GridCellCm, gy * GridCellCm);

			// 원형 맵 범위 확인
			if (CellCenter.Size() > RadiusCm) continue;

			for (int32 d = 0; d < 8; ++d)
			{
				TotalDirs++;
				bool bBlocked = false;

				for (const FBuildingAnalysis& B : CachedBuildings)
				{
					if (B.HeightM < MinBlockHeightM) continue;

					const FVector2D Delta = B.CentroidUE5 - CellCenter;
					const float Dist = Delta.Size();
					if (Dist < 1.f || Dist > CheckRangeCm) continue;

					const FVector2D DirNorm = Delta / Dist;
					const float Dot = FVector2D::DotProduct(DirNorm, kDirections8[d]);
					if (Dot > kCosConeHalf)
					{
						bBlocked = true;
						break;
					}
				}

				if (!bBlocked) OpenDirs++;
			}
		}
	}

	return (TotalDirs > 0)
		? (static_cast<float>(OpenDirs) / static_cast<float>(TotalDirs) * 100.f)
		: 100.f;
}

// ── 동선 복잡도: min(100, 평균_노드_degree / 4 × 100) ──────────────────────
float UDesignerIntentSubsystem::ComputeRouteComplexity() const
{
	if (CachedRoads.Num() == 0) return 0.f;

	// 도로 끝점 스냅 → 노드 도출 (10m = 1000cm 셀)
	const float SnapCellCm = 1000.f;
	TMap<int64, int32> NodeDegrees;   // cellKey → 연결 수

	auto CellKey = [SnapCellCm](const FVector2D& Pt) -> int64
	{
		int32 cx = FMath::FloorToInt(Pt.X / SnapCellCm);
		int32 cy = FMath::FloorToInt(Pt.Y / SnapCellCm);
		return (static_cast<int64>(cx) << 32) | static_cast<int64>(static_cast<uint32>(cy));
	};

	for (const FRoadAnalysis& Road : CachedRoads)
	{
		if (Road.PointsUE5.Num() < 2) continue;
		const int64 KeyA = CellKey(Road.PointsUE5[0]);
		const int64 KeyB = CellKey(Road.PointsUE5.Last());
		NodeDegrees.FindOrAdd(KeyA)++;
		NodeDegrees.FindOrAdd(KeyB)++;
	}

	if (NodeDegrees.Num() == 0) return 0.f;

	float SumDegree = 0.f;
	for (const auto& KV : NodeDegrees)
	{
		SumDegree += static_cast<float>(KV.Value);
	}
	const float AvgDegree = SumDegree / static_cast<float>(NodeDegrees.Num());
	return FMath::Min(100.f, AvgDegree / 4.f * 100.f);
}

// ── 고저차: min(100, (최고-최저) / (반경_m × 0.1) × 100) ───────────────────
float UDesignerIntentSubsystem::ComputeElevationContrast() const
{
	const float ElevDiff = ElevationMaxM - ElevationMinM;
	if (ElevDiff <= 0.f || MapRadiusKm <= 0.f) return 0.f;

	const float RadiusM = MapRadiusKm * 1000.f;
	const float Raw = ElevDiff / (RadiusM * 0.1f) * 100.f;
	return FMath::Min(100.f, Raw);
}

// ── 파괴도: 1단계 OSM 데이터는 모두 intact → 항상 0 ─────────────────────────
float UDesignerIntentSubsystem::ComputeDestructionLevel() const
{
	return 0.f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  달성률 한계 — 지형 유형별 테이블
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::ComputeAchievementLimits()
{
	float DensityMax = 100.f, OpennessMax = 100.f, RouteMax = 100.f;
	FString Reason;

	if (TerrainType == TEXT("urban_dense"))
	{
		DensityMax  = 60.f;
		OpennessMax = 80.f;
		RouteMax    = 95.f;
		Reason = TEXT("도심 밀집 지역 — 추가 배치 공간 제한");
	}
	else if (TerrainType == TEXT("mountain"))
	{
		DensityMax  = 70.f;
		OpennessMax = 60.f;
		RouteMax    = 80.f;
		Reason = TEXT("산악 지형 — 급경사/수역으로 배치 불가 영역 다수");
	}
	else // mixed, suburban, coastal 등
	{
		DensityMax  = 95.f;
		OpennessMax = 95.f;
		RouteMax    = 90.f;
		Reason = TEXT("");
	}

	auto SetLimit = [this](ESliderType T, float Max, const FString& R)
	{
		FSliderState& S = SliderStates.FindOrAdd(T);
		S.AchievementLimitMax = Max;
		S.LimitReason = R;
	};

	SetLimit(ESliderType::UrbanDensity,     DensityMax,  Reason);
	SetLimit(ESliderType::Openness,         OpennessMax, Reason);
	SetLimit(ESliderType::RouteComplexity,  RouteMax,    Reason);
	SetLimit(ESliderType::ElevationContrast, 100.f,      TEXT(""));
	SetLimit(ESliderType::DestructionLevel,  100.f,      TEXT(""));
}

// ─────────────────────────────────────────────────────────────────────────────
//  map_meta.json 읽기/쓰기
// ─────────────────────────────────────────────────────────────────────────────

FString UDesignerIntentSubsystem::GetMapMetaPath() const
{
	return FPaths::ProjectSavedDir() / TEXT("LevelTool") / TEXT("EditLayers")
		/ ActiveMapId / TEXT("map_meta.json");
}

bool UDesignerIntentSubsystem::LoadMapMetaData(const FString& MetaPath)
{
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *MetaPath)) return false;

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;

	const TSharedPtr<FJsonObject>* OriginObj;
	if (Root->TryGetObjectField(TEXT("origin"), OriginObj))
	{
		MapRadiusKm = (*OriginObj)->GetNumberField(TEXT("radius_km"));
	}

	const TSharedPtr<FJsonObject>* TerrainObj;
	if (Root->TryGetObjectField(TEXT("terrain_profile"), TerrainObj))
	{
		ElevationMinM = (*TerrainObj)->GetNumberField(TEXT("min_elevation_m"));
		ElevationMaxM = (*TerrainObj)->GetNumberField(TEXT("max_elevation_m"));
		TerrainType   = (*TerrainObj)->GetStringField(TEXT("type"));
	}

	return true;
}

void UDesignerIntentSubsystem::SaveSliderValuesToMapMeta() const
{
	const FString MetaPath = GetMapMetaPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *MetaPath)) return;

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	auto GetInit = [this](ESliderType T) -> float
	{
		const FSliderState* S = SliderStates.Find(T);
		return S ? S->InitialValue : 0.f;
	};
	auto GetLimit = [this](ESliderType T) -> float
	{
		const FSliderState* S = SliderStates.Find(T);
		return S ? S->AchievementLimitMax : 100.f;
	};

	// slider_initial_values
	auto SliderObj = MakeShared<FJsonObject>();
	SliderObj->SetNumberField(TEXT("urban_density"),      GetInit(ESliderType::UrbanDensity));
	SliderObj->SetNumberField(TEXT("openness"),           GetInit(ESliderType::Openness));
	SliderObj->SetNumberField(TEXT("route_complexity"),   GetInit(ESliderType::RouteComplexity));
	SliderObj->SetNumberField(TEXT("elevation_contrast"), GetInit(ESliderType::ElevationContrast));
	SliderObj->SetNumberField(TEXT("destruction_level"),  GetInit(ESliderType::DestructionLevel));
	Root->SetObjectField(TEXT("slider_initial_values"), SliderObj);

	// terrain_achievement_limits
	auto LimitsObj = MakeShared<FJsonObject>();
	LimitsObj->SetNumberField(TEXT("urban_density_max"),      GetLimit(ESliderType::UrbanDensity));
	LimitsObj->SetNumberField(TEXT("openness_max"),           GetLimit(ESliderType::Openness));
	LimitsObj->SetNumberField(TEXT("route_complexity_max"),   GetLimit(ESliderType::RouteComplexity));
	LimitsObj->SetNumberField(TEXT("elevation_contrast_max"), GetLimit(ESliderType::ElevationContrast));
	LimitsObj->SetNumberField(TEXT("destruction_level_max"),  GetLimit(ESliderType::DestructionLevel));
	{
		const FSliderState* S = SliderStates.Find(ESliderType::UrbanDensity);
		LimitsObj->SetStringField(TEXT("limit_reason"), S ? S->LimitReason : FString());
	}
	Root->SetObjectField(TEXT("terrain_achievement_limits"), LimitsObj);

	Root->SetStringField(TEXT("updated_at"), FDateTime::UtcNow().ToIso8601());

	FString Output;
	auto Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FFileHelper::SaveStringToFile(Output, *MetaPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	UE_LOG(LogDesignerIntent, Log, TEXT("slider_initial_values → %s"), *MetaPath);
}

// ─────────────────────────────────────────────────────────────────────────────
//  예측 이력 영속화 → map_meta.json
// ─────────────────────────────────────────────────────────────────────────────

void UDesignerIntentSubsystem::SavePredictionHistoryToMeta() const
{
	const FString MetaPath = GetMapMetaPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *MetaPath)) return;

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FPredictionSample& S : PredictionHistory)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("predicted"), S.Predicted);
		Obj->SetNumberField(TEXT("actual"), S.Actual);
		Obj->SetNumberField(TEXT("error"), S.Error);
		Obj->SetStringField(TEXT("timestamp"), S.Timestamp.ToIso8601());
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto PredObj = MakeShared<FJsonObject>();
	PredObj->SetNumberField(TEXT("bias"), PredictionBias);
	PredObj->SetNumberField(TEXT("scale"), PredictionScale);
	PredObj->SetArrayField(TEXT("history"), Arr);
	Root->SetObjectField(TEXT("prediction_correction"), PredObj);

	FString Output;
	auto Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(Output, *MetaPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void UDesignerIntentSubsystem::LoadPredictionHistoryFromMeta()
{
	const FString MetaPath = GetMapMetaPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *MetaPath)) return;

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	const TSharedPtr<FJsonObject>* PredObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("prediction_correction"), PredObj)) return;

	PredictionBias  = (*PredObj)->GetNumberField(TEXT("bias"));
	PredictionScale = (*PredObj)->GetNumberField(TEXT("scale"));

	PredictionHistory.Empty();
	const TArray<TSharedPtr<FJsonValue>>& Arr = (*PredObj)->GetArrayField(TEXT("history"));
	for (const auto& V : Arr)
	{
		const TSharedPtr<FJsonObject>& Obj = V->AsObject();
		if (!Obj) continue;
		FPredictionSample S;
		S.Predicted = Obj->GetNumberField(TEXT("predicted"));
		S.Actual    = Obj->GetNumberField(TEXT("actual"));
		S.Error     = Obj->GetNumberField(TEXT("error"));
		FDateTime::ParseIso8601(*Obj->GetStringField(TEXT("timestamp")), S.Timestamp);
		PredictionHistory.Add(MoveTemp(S));
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("예측 이력 복원: %d건, bias=%.2f, scale=%.2f"),
		PredictionHistory.Num(), PredictionBias, PredictionScale);
}

float UDesignerIntentSubsystem::GetPredictionErrorMargin() const
{
	if (PredictionHistory.Num() < 3) return 5.f;

	int32 WindowSize = FMath::Min(PredictionHistory.Num(), 10);
	float SumAbsErr = 0.f;
	for (int32 i = PredictionHistory.Num() - WindowSize; i < PredictionHistory.Num(); ++i)
		SumAbsErr += FMath::Abs(PredictionHistory[i].Error);

	float MAE = SumAbsErr / WindowSize;
	return FMath::Clamp(MAE * 1.5f, 2.f, 15.f);
}

float UDesignerIntentSubsystem::GetCollisionRate() const
{
	if (PredictionHistory.Num() < 2) return 0.f;

	int32 WindowSize = FMath::Min(PredictionHistory.Num(), 20);
	int32 Collisions = 0;
	float Threshold = 5.f;

	for (int32 i = PredictionHistory.Num() - WindowSize; i < PredictionHistory.Num(); ++i)
	{
		if (FMath::Abs(PredictionHistory[i].Error) > Threshold)
			++Collisions;
	}

	return (float)Collisions / WindowSize * 100.f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  데이터 로딩 — buildings.json
// ─────────────────────────────────────────────────────────────────────────────

bool UDesignerIntentSubsystem::LoadBuildingData(const FString& JsonPath)
{
	CachedBuildings.Empty();
	if (JsonPath.IsEmpty() || !FPaths::FileExists(JsonPath)) return false;

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return false;

	TArray<TSharedPtr<FJsonValue>> Arr;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Arr)) return false;

	CachedBuildings.Reserve(Arr.Num());
	for (const auto& Val : Arr)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj) continue;

		FBuildingAnalysis B;
		B.OsmId   = static_cast<int64>(Obj->GetNumberField(TEXT("id")));
		B.TypeKey = Obj->GetStringField(TEXT("type"));
		B.HeightM = static_cast<float>(Obj->GetNumberField(TEXT("height_m")));
		B.AreaM2  = static_cast<float>(Obj->GetNumberField(TEXT("area_m2")));

		const TArray<TSharedPtr<FJsonValue>>& Centroid =
			Obj->GetArrayField(TEXT("centroid_ue5"));
		if (Centroid.Num() >= 2)
		{
			B.CentroidUE5.X = static_cast<float>(Centroid[0]->AsNumber());
			B.CentroidUE5.Y = static_cast<float>(Centroid[1]->AsNumber());
		}

		CachedBuildings.Add(MoveTemp(B));
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("건물 %d개 로드: %s"),
		CachedBuildings.Num(), *JsonPath);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  데이터 로딩 — roads.json
// ─────────────────────────────────────────────────────────────────────────────

bool UDesignerIntentSubsystem::LoadRoadData(const FString& JsonPath)
{
	CachedRoads.Empty();
	if (JsonPath.IsEmpty() || !FPaths::FileExists(JsonPath)) return false;

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return false;

	TArray<TSharedPtr<FJsonValue>> Arr;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Arr)) return false;

	CachedRoads.Reserve(Arr.Num());
	for (const auto& Val : Arr)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj) continue;

		FRoadAnalysis R;
		R.OsmId    = static_cast<int64>(Obj->GetNumberField(TEXT("id")));
		R.RoadType = Obj->GetStringField(TEXT("type"));

		const TArray<TSharedPtr<FJsonValue>>& Pts =
			Obj->GetArrayField(TEXT("points_ue5"));
		R.PointsUE5.Reserve(Pts.Num());
		for (const auto& PtVal : Pts)
		{
			const TArray<TSharedPtr<FJsonValue>>& Coord = PtVal->AsArray();
			if (Coord.Num() >= 2)
			{
				R.PointsUE5.Add(FVector2D(
					static_cast<float>(Coord[0]->AsNumber()),
					static_cast<float>(Coord[1]->AsNumber())));
			}
		}

		CachedRoads.Add(MoveTemp(R));
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("도로 %d개 로드: %s"),
		CachedRoads.Num(), *JsonPath);
	return true;
}

bool UDesignerIntentSubsystem::LoadWaterData(const FString& JsonPath)
{
	CachedWaterAreas.Empty();
	if (JsonPath.IsEmpty() || !FPaths::FileExists(JsonPath)) return false;

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return false;

	TArray<TSharedPtr<FJsonValue>> Arr;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Arr)) return false;

	CachedWaterAreas.Reserve(Arr.Num());
	for (const auto& Val : Arr)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj) continue;

		FWaterArea W;
		const TArray<TSharedPtr<FJsonValue>>& Pts = Obj->GetArrayField(TEXT("polygon_ue5"));
		FVector2D Sum = FVector2D::ZeroVector;
		float MaxDist = 0.f;

		for (const auto& PtVal : Pts)
		{
			const TArray<TSharedPtr<FJsonValue>>& Coord = PtVal->AsArray();
			if (Coord.Num() >= 2)
			{
				FVector2D P(static_cast<float>(Coord[0]->AsNumber()),
				            static_cast<float>(Coord[1]->AsNumber()));
				W.Polygon.Add(P);
				Sum += P;
			}
		}

		if (W.Polygon.Num() > 0)
		{
			W.Center = Sum / W.Polygon.Num();
			for (const FVector2D& P : W.Polygon)
				MaxDist = FMath::Max(MaxDist, FVector2D::Distance(W.Center, P));
			W.RadiusCm = MaxDist;
			CachedWaterAreas.Add(MoveTemp(W));
		}
	}

	UE_LOG(LogDesignerIntent, Log, TEXT("수역 %d개 로드: %s"),
		CachedWaterAreas.Num(), *JsonPath);
	return true;
}
