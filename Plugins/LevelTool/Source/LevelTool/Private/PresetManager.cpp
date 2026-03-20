#include "PresetManager.h"
#include "DesignerIntentSubsystem.h"
#include "DesignerIntentTypes.h"
#include "ChecklistEngine.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFilemanager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPresetMgr, Log, All);

// 적용 순서: ① 고저차 → ② 파괴도 → ③ 도심 밀도 → ④ 개방도 → ⑤ 동선 복잡도
static const ESliderType kApplyOrder[] =
{
	ESliderType::ElevationContrast,
	ESliderType::DestructionLevel,
	ESliderType::UrbanDensity,
	ESliderType::Openness,
	ESliderType::RouteComplexity,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization
// ─────────────────────────────────────────────────────────────────────────────

void UPresetManager::Initialize(UDesignerIntentSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
	ReferencePresets = LevelToolPresets::GetBuiltInPresets();
	LoadCustomPresets();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Query
// ─────────────────────────────────────────────────────────────────────────────

TArray<FMapPreset> UPresetManager::GetAllPresets() const
{
	TArray<FMapPreset> All;
	All.Append(ReferencePresets);
	All.Append(CustomPresets);
	return All;
}

const FMapPreset* UPresetManager::FindPreset(const FString& PresetName) const
{
	for (const FMapPreset& P : ReferencePresets)
	{
		if (P.PresetName == PresetName) return &P;
	}
	for (const FMapPreset& P : CustomPresets)
	{
		if (P.PresetName == PresetName) return &P;
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply — 5개 슬라이더를 적용 순서대로 일괄 설정
// ─────────────────────────────────────────────────────────────────────────────

bool UPresetManager::ApplyPreset(const FString& PresetName)
{
	const FMapPreset* Preset = FindPreset(PresetName);
	if (!Preset)
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("프리셋 '%s'을 찾을 수 없습니다"), *PresetName);
		return false;
	}

	if (!Subsystem)
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("DesignerIntentSubsystem 없음"));
		return false;
	}

	UE_LOG(LogPresetMgr, Log, TEXT("프리셋 적용: '%s' (%s — %s)"),
		*Preset->PresetName, *Preset->ReferenceGame, *Preset->ReferenceMap);

	bool bHasLandscape = false;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			bHasLandscape = true;
			break;
		}
	}

	if (!bHasLandscape)
	{
		EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
			FText::FromString(TEXT("Landscape가 존재하지 않습니다.\n지형 관련 슬라이더(고저차·개방감)가 제한됩니다.\n\n계속 적용하시겠습니까?")));

		if (Answer == EAppReturnType::No)
		{
			UE_LOG(LogPresetMgr, Log, TEXT("프리셋 적용 취소 (Landscape 미존재)"));
			return false;
		}
	}

	Subsystem->BeginBatchApply();
	for (ESliderType Type : kApplyOrder)
	{
		const float Target = Preset->Sliders.Get(Type);
		Subsystem->ApplySlider(Type, Target);
	}
	Subsystem->EndBatchApply();

	if (Preset->bIsReference)
	{
		LastAppliedRuleset = Preset->Ruleset;

		if (UChecklistEngine* CE = Subsystem->GetChecklistEngine())
		{
			CE->RunDiagnosis(Preset->Ruleset);
			UE_LOG(LogPresetMgr, Log, TEXT("레퍼런스 프리셋 → 룰셋 %s 자동 진단 실행"),
				Preset->Ruleset == ERulesetType::BR ? TEXT("BR") : TEXT("EX"));
		}
	}

	UE_LOG(LogPresetMgr, Log, TEXT("프리셋 '%s' 적용 완료"), *PresetName);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Custom preset CRUD
// ─────────────────────────────────────────────────────────────────────────────

bool UPresetManager::SaveCustomPreset(const FMapPreset& Preset)
{
	FString Json;
	if (!SerializePresetToJson(Preset, Json)) return false;

	const FString FilePath = GetPresetFilePath(Preset.PresetName);
	const FString Dir = FPaths::GetPath(FilePath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir)) PF.CreateDirectoryTree(*Dir);

	if (!FFileHelper::SaveStringToFile(Json, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogPresetMgr, Error, TEXT("커스텀 프리셋 저장 실패: %s"), *FilePath);
		return false;
	}

	// 목록 갱신
	CustomPresets.RemoveAll([&](const FMapPreset& P) { return P.PresetName == Preset.PresetName; });
	CustomPresets.Add(Preset);
	OnPresetsChanged.Broadcast();

	UE_LOG(LogPresetMgr, Log, TEXT("커스텀 프리셋 저장: %s"), *FilePath);
	return true;
}

bool UPresetManager::DeleteCustomPreset(const FString& PresetName)
{
	const FString FilePath = GetPresetFilePath(PresetName);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

	if (PF.FileExists(*FilePath))
	{
		PF.DeleteFile(*FilePath);
	}

	const int32 Removed = CustomPresets.RemoveAll(
		[&](const FMapPreset& P) { return P.PresetName == PresetName; });

	if (Removed > 0) OnPresetsChanged.Broadcast();
	return Removed > 0;
}

bool UPresetManager::ImportPreset(const FString& FilePath)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("가져오기 실패: 파일 로드 불가 %s"), *FilePath);
		return false;
	}

	FMapPreset Preset;
	if (!DeserializePresetFromJson(Json, Preset))
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("가져오기 1단계 실패: JSON 파싱 오류"));
		return false;
	}

	if (Preset.PresetName.IsEmpty())
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("가져오기 2단계 실패: 프리셋 이름 누락"));
		return false;
	}
	const float SliderArr[] = {
		Preset.Sliders.UrbanDensity, Preset.Sliders.Openness,
		Preset.Sliders.RouteComplexity, Preset.Sliders.ElevationContrast,
		Preset.Sliders.DestructionLevel
	};
	static const TCHAR* SliderNames[] = {
		TEXT("UrbanDensity"), TEXT("Openness"), TEXT("RouteComplexity"),
		TEXT("ElevationContrast"), TEXT("DestructionLevel")
	};
	for (int32 i = 0; i < 5; ++i)
	{
		if (SliderArr[i] < 0.f || SliderArr[i] > 100.f)
		{
			UE_LOG(LogPresetMgr, Warning, TEXT("가져오기 3단계 실패: %s 값 범위 초과 (%.1f)"),
				SliderNames[i], SliderArr[i]);
			return false;
		}
	}

	if (FindPreset(Preset.PresetName) != nullptr)
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("가져오기 4단계 경고: 동일 이름 '%s' 덮어쓰기"),
			*Preset.PresetName);
	}

	Preset.bIsReference = false;
	return SaveCustomPreset(Preset);
}

bool UPresetManager::ExportPreset(const FString& PresetName, const FString& DestPath) const
{
	const FMapPreset* Preset = FindPreset(PresetName);
	if (!Preset) return false;

	FString Json;
	if (!SerializePresetToJson(*Preset, Json)) return false;

	return FFileHelper::SaveStringToFile(Json, *DestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FMapPreset UPresetManager::MakePresetFromCurrentState(
	const FString& Name, const FString& Author, const FString& Desc) const
{
	FMapPreset P;
	P.PresetName   = Name;
	P.Author       = Author;
	P.Description  = Desc;
	P.bIsReference = false;
	P.CreatedAt    = FDateTime::UtcNow().ToIso8601();

	if (Subsystem)
	{
		const auto& States = Subsystem->GetAllSliderStates();
		if (const FSliderState* S = States.Find(ESliderType::UrbanDensity))
			P.Sliders.UrbanDensity = S->CurrentValue;
		if (const FSliderState* S = States.Find(ESliderType::Openness))
			P.Sliders.Openness = S->CurrentValue;
		if (const FSliderState* S = States.Find(ESliderType::RouteComplexity))
			P.Sliders.RouteComplexity = S->CurrentValue;
		if (const FSliderState* S = States.Find(ESliderType::ElevationContrast))
			P.Sliders.ElevationContrast = S->CurrentValue;
		if (const FSliderState* S = States.Find(ESliderType::DestructionLevel))
			P.Sliders.DestructionLevel = S->CurrentValue;
	}

	return P;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Load all custom presets from disk
// ─────────────────────────────────────────────────────────────────────────────

void UPresetManager::LoadCustomPresets()
{
	CustomPresets.Empty();

	const FString Dir = GetCustomPresetsDir();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir)) return;

	TArray<FString> Files;
	PF.FindFilesRecursively(Files, *Dir, TEXT(".json"));

	for (const FString& File : Files)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *File)) continue;

		FMapPreset Preset;
		if (DeserializePresetFromJson(Json, Preset))
		{
			Preset.bIsReference = false;
			CustomPresets.Add(MoveTemp(Preset));
		}
	}

	UE_LOG(LogPresetMgr, Log, TEXT("커스텀 프리셋 %d개 로드"), CustomPresets.Num());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Path helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UPresetManager::GetCustomPresetsDir()
{
	return FPaths::ProjectSavedDir() / TEXT("LevelTool") / TEXT("CustomPresets");
}

FString UPresetManager::GetPresetFilePath(const FString& PresetName)
{
	FString SafeName = PresetName;
	SafeName.ReplaceInline(TEXT(" "), TEXT("_"));
	SafeName.ReplaceInline(TEXT("/"), TEXT("_"));
	return GetCustomPresetsDir() / SafeName + TEXT(".json");
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON serialization
// ─────────────────────────────────────────────────────────────────────────────

bool UPresetManager::SerializePresetToJson(const FMapPreset& P, FString& OutJson) const
{
	auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("preset_name"),    P.PresetName);
	Root->SetStringField(TEXT("reference_game"), P.ReferenceGame);
	Root->SetStringField(TEXT("reference_map"),  P.ReferenceMap);

	auto Coords = MakeShared<FJsonObject>();
	Coords->SetNumberField(TEXT("lat"),       P.RecommendedCoords.Lat);
	Coords->SetNumberField(TEXT("lon"),       P.RecommendedCoords.Lon);
	Coords->SetNumberField(TEXT("radius_km"), P.RecommendedCoords.RadiusKm);
	Coords->SetStringField(TEXT("description"), P.RecommendedCoords.Description);
	Root->SetObjectField(TEXT("recommended_coords"), Coords);

	auto Sliders = MakeShared<FJsonObject>();
	Sliders->SetNumberField(TEXT("urban_density"),      P.Sliders.UrbanDensity);
	Sliders->SetNumberField(TEXT("openness"),            P.Sliders.Openness);
	Sliders->SetNumberField(TEXT("route_complexity"),    P.Sliders.RouteComplexity);
	Sliders->SetNumberField(TEXT("elevation_emphasis"),  P.Sliders.ElevationContrast);
	Sliders->SetNumberField(TEXT("destruction"),         P.Sliders.DestructionLevel);
	Root->SetObjectField(TEXT("sliders"), Sliders);

	Root->SetStringField(TEXT("ruleset"), RulesetToString(P.Ruleset));

	if (!P.Author.IsEmpty())
		Root->SetStringField(TEXT("author"), P.Author);
	if (!P.CreatedAt.IsEmpty())
		Root->SetStringField(TEXT("created_at"), P.CreatedAt);
	if (!P.BaseReference.IsEmpty())
		Root->SetStringField(TEXT("base_reference"), P.BaseReference);
	if (!P.Description.IsEmpty())
		Root->SetStringField(TEXT("description"), P.Description);

	if (P.ObjectPresetsApplied.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : P.ObjectPresetsApplied)
			Arr.Add(MakeShared<FJsonValueString>(S));
		Root->SetArrayField(TEXT("object_presets_applied"), Arr);
	}

	auto Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	return FJsonSerializer::Serialize(Root, Writer);
}

bool UPresetManager::DeserializePresetFromJson(const FString& Json, FMapPreset& Out) const
{
	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogPresetMgr, Warning, TEXT("프리셋 JSON 파싱 실패"));
		return false;
	}

	Out.PresetName    = Root->GetStringField(TEXT("preset_name"));
	Out.ReferenceGame = Root->GetStringField(TEXT("reference_game"));
	Out.ReferenceMap  = Root->GetStringField(TEXT("reference_map"));

	const TSharedPtr<FJsonObject>* CoordsObj;
	if (Root->TryGetObjectField(TEXT("recommended_coords"), CoordsObj))
	{
		Out.RecommendedCoords.Lat       = (*CoordsObj)->GetNumberField(TEXT("lat"));
		Out.RecommendedCoords.Lon       = (*CoordsObj)->GetNumberField(TEXT("lon"));
		Out.RecommendedCoords.RadiusKm  = (*CoordsObj)->GetNumberField(TEXT("radius_km"));
		Out.RecommendedCoords.Description = (*CoordsObj)->GetStringField(TEXT("description"));
	}

	const TSharedPtr<FJsonObject>* SlidersObj;
	if (Root->TryGetObjectField(TEXT("sliders"), SlidersObj))
	{
		Out.Sliders.UrbanDensity      = FMath::Clamp((float)(*SlidersObj)->GetNumberField(TEXT("urban_density")), 0.f, 100.f);
		Out.Sliders.Openness          = FMath::Clamp((float)(*SlidersObj)->GetNumberField(TEXT("openness")), 0.f, 100.f);
		Out.Sliders.RouteComplexity   = FMath::Clamp((float)(*SlidersObj)->GetNumberField(TEXT("route_complexity")), 0.f, 100.f);
		Out.Sliders.ElevationContrast = FMath::Clamp((float)(*SlidersObj)->GetNumberField(TEXT("elevation_emphasis")), 0.f, 100.f);
		Out.Sliders.DestructionLevel  = FMath::Clamp((float)(*SlidersObj)->GetNumberField(TEXT("destruction")), 0.f, 100.f);
	}

	Out.Ruleset = StringToRuleset(Root->GetStringField(TEXT("ruleset")));

	Out.Author        = Root->GetStringField(TEXT("author"));
	Out.CreatedAt     = Root->GetStringField(TEXT("created_at"));
	Out.BaseReference = Root->GetStringField(TEXT("base_reference"));
	Out.Description   = Root->GetStringField(TEXT("description"));

	const TArray<TSharedPtr<FJsonValue>>* ArrPtr;
	if (Root->TryGetArrayField(TEXT("object_presets_applied"), ArrPtr))
	{
		for (const auto& V : *ArrPtr)
			Out.ObjectPresetsApplied.Add(V->AsString());
	}

	return !Out.PresetName.IsEmpty();
}

// ─────────────────────────────────────────────────────────────────────────────
//  오브젝트 프리셋 5종 — 위치 기반 구조물 패턴 배치
// ─────────────────────────────────────────────────────────────────────────────

bool UPresetManager::ApplyObjectPreset(EObjectPresetType Type, const FVector2D& Center)
{
	if (!Subsystem) return false;

	UEditLayerManager* LM = Subsystem->GetLayerManager();
	if (!LM) return false;

	auto MakeLayer = [](EEditLayerType LType, const FString& Label, const FVector2D& Loc) -> FEditLayer
	{
		FEditLayer L;
		L.Type      = LType;
		L.CreatedBy = ELayerCreatedBy::Preset;
		L.Label     = Label;
		L.Area.Type = EAreaType::Point;
		L.Area.Point.LocationUE5 = FVector(Loc.X, Loc.Y, 0.f);
		L.Params = MakeShared<FJsonObject>();
		return L;
	};

	TArray<FEditLayer> Layers;

	switch (Type)
	{
	case EObjectPresetType::SniperPoint:
	{
		FEditLayer Tower = MakeLayer(EEditLayerType::BuildingAdd, TEXT("[프리셋] 저격 타워"), Center);
		Tower.Params->SetStringField(TEXT("type_key"), TEXT("Tower_Observation"));
		Tower.Params->SetNumberField(TEXT("height_m"), 25.f);
		Tower.Params->SetNumberField(TEXT("footprint_m2"), 36.f);
		Tower.Params->SetNumberField(TEXT("aspect_ratio"), 1.0);
		Tower.Params->SetNumberField(TEXT("rotation_deg"), 0.f);
		Tower.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));
		Layers.Add(MoveTemp(Tower));

		for (int32 i = 0; i < 3; ++i)
		{
			const float Angle = i * 120.f;
			FVector2D Off(FMath::Cos(FMath::DegreesToRadians(Angle)) * 2000.f,
			              FMath::Sin(FMath::DegreesToRadians(Angle)) * 2000.f);
			FEditLayer Cover = MakeLayer(EEditLayerType::BuildingAdd,
				FString::Printf(TEXT("[프리셋] 저격 엄폐 %d"), i + 1), Center + Off);
			Cover.Params->SetStringField(TEXT("type_key"), TEXT("Wall_Low"));
			Cover.Params->SetNumberField(TEXT("height_m"), 1.5f);
			Cover.Params->SetNumberField(TEXT("footprint_m2"), 12.f);
			Cover.Params->SetNumberField(TEXT("aspect_ratio"), 4.f);
			Cover.Params->SetNumberField(TEXT("rotation_deg"), Angle);
			Cover.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));
			Layers.Add(MoveTemp(Cover));
		}

		FEditLayer Marker = MakeLayer(EEditLayerType::Marker, TEXT("[프리셋] 저격 포인트 마커"), Center);
		Marker.Params->SetStringField(TEXT("marker_type"), TEXT("sniper_point"));
		Layers.Add(MoveTemp(Marker));
		break;
	}

	case EObjectPresetType::CentralPlaza:
	{
		for (int32 i = 0; i < 6; ++i)
		{
			const float Angle = i * 60.f;
			FVector2D Pos(Center.X + FMath::Cos(FMath::DegreesToRadians(Angle)) * 5000.f,
			              Center.Y + FMath::Sin(FMath::DegreesToRadians(Angle)) * 5000.f);
			FEditLayer B = MakeLayer(EEditLayerType::BuildingAdd,
				FString::Printf(TEXT("[프리셋] 광장 주변 건물 %d"), i + 1), Pos);
			B.Params->SetStringField(TEXT("type_key"), TEXT("Commercial_Low"));
			B.Params->SetNumberField(TEXT("height_m"), 6.f + FMath::FRandRange(0.f, 4.f));
			B.Params->SetNumberField(TEXT("footprint_m2"), 150.f);
			B.Params->SetNumberField(TEXT("aspect_ratio"), 1.3f);
			B.Params->SetNumberField(TEXT("rotation_deg"), Angle + 90.f);
			B.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));
			Layers.Add(MoveTemp(B));
		}

		FEditLayer Marker = MakeLayer(EEditLayerType::Marker, TEXT("[프리셋] 중앙 광장 마커"), Center);
		Marker.Params->SetStringField(TEXT("marker_type"), TEXT("poi_center"));
		Layers.Add(MoveTemp(Marker));
		break;
	}

	case EObjectPresetType::NarrowEntry:
	{
		const FVector2D Left(Center.X - 800.f, Center.Y);
		const FVector2D Right(Center.X + 800.f, Center.Y);

		for (int32 side = 0; side < 2; ++side)
		{
			FVector2D Pos = (side == 0) ? Left : Right;
			FEditLayer Wall = MakeLayer(EEditLayerType::BuildingAdd,
				FString::Printf(TEXT("[프리셋] 좁은 진입로 벽 %d"), side + 1), Pos);
			Wall.Params->SetStringField(TEXT("type_key"), TEXT("Industrial_Wall"));
			Wall.Params->SetNumberField(TEXT("height_m"), 8.f);
			Wall.Params->SetNumberField(TEXT("footprint_m2"), 200.f);
			Wall.Params->SetNumberField(TEXT("aspect_ratio"), 5.f);
			Wall.Params->SetNumberField(TEXT("rotation_deg"), 0.f);
			Wall.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));
			Layers.Add(MoveTemp(Wall));
		}

		FEditLayer Cover = MakeLayer(EEditLayerType::BuildingAdd, TEXT("[프리셋] 진입로 엄폐물"), Center);
		Cover.Params->SetStringField(TEXT("type_key"), TEXT("Debris"));
		Cover.Params->SetNumberField(TEXT("height_m"), 2.f);
		Cover.Params->SetNumberField(TEXT("footprint_m2"), 16.f);
		Cover.Params->SetNumberField(TEXT("aspect_ratio"), 2.f);
		Cover.Params->SetNumberField(TEXT("rotation_deg"), 45.f);
		Cover.Params->SetStringField(TEXT("destruction_state"), TEXT("partial"));
		Layers.Add(MoveTemp(Cover));
		break;
	}

	case EObjectPresetType::ChokepointBypass:
	{
		FEditLayer Block = MakeLayer(EEditLayerType::RoadBlock, TEXT("[프리셋] 병목 차단"), Center);
		Block.Params->SetStringField(TEXT("obstacle_type"), TEXT("barricade"));
		Block.Params->SetBoolField(TEXT("passable_on_foot"), true);
		Layers.Add(MoveTemp(Block));

		FVector2D BypassStart(Center.X - 3000.f, Center.Y - 5000.f);
		FVector2D BypassEnd(Center.X + 3000.f, Center.Y + 5000.f);

		FEditLayer Road;
		Road.Type      = EEditLayerType::RoadAdd;
		Road.CreatedBy = ELayerCreatedBy::Preset;
		Road.Label     = TEXT("[프리셋] 우회 도로");
		Road.Area.Type = EAreaType::Path;
		Road.Area.Path.PointsUE5 = { BypassStart, Center + FVector2D(5000.f, 0.f), BypassEnd };
		Road.Area.Path.WidthCm   = 400.f;
		Road.Params = MakeShared<FJsonObject>();
		Road.Params->SetStringField(TEXT("road_type"), TEXT("minor"));
		Road.Params->SetNumberField(TEXT("width_m"), 4.0);
		Layers.Add(MoveTemp(Road));
		break;
	}

	case EObjectPresetType::VerticalCombat:
	{
		const float Heights[] = { 20.f, 15.f, 12.f, 25.f };
		const FVector2D Offsets[] = {
			FVector2D(0, 0), FVector2D(2500.f, 0), FVector2D(0, 2500.f), FVector2D(2500.f, 2500.f)
		};

		for (int32 i = 0; i < 4; ++i)
		{
			FEditLayer B = MakeLayer(EEditLayerType::BuildingAdd,
				FString::Printf(TEXT("[프리셋] 수직전투 건물 %d"), i + 1), Center + Offsets[i]);
			B.Params->SetStringField(TEXT("type_key"), TEXT("HighRise_Residential"));
			B.Params->SetNumberField(TEXT("height_m"), Heights[i]);
			B.Params->SetNumberField(TEXT("footprint_m2"), 200.f + i * 50.f);
			B.Params->SetNumberField(TEXT("aspect_ratio"), 1.2f);
			B.Params->SetNumberField(TEXT("rotation_deg"), i * 15.f);
			B.Params->SetStringField(TEXT("destruction_state"), TEXT("intact"));
			Layers.Add(MoveTemp(B));
		}

		FEditLayer Marker = MakeLayer(EEditLayerType::Marker, TEXT("[프리셋] 수직 전투 구역 마커"), Center);
		Marker.Params->SetStringField(TEXT("marker_type"), TEXT("vertical_combat"));
		Layers.Add(MoveTemp(Marker));
		break;
	}
	}

	const FString SourceId = FString::Printf(TEXT("object_preset_%d_%lld"),
		(int32)Type, FDateTime::UtcNow().GetTicks());

	for (FEditLayer& L : Layers)
	{
		L.SourceId = SourceId;
		LM->AddLayer(MoveTemp(L));
	}

	if (UEditLayerApplicator* App = LM->GetApplicator())
	{
		for (FEditLayer* L : LM->FindLayersBySourceId(SourceId))
		{
			App->ApplyLayer(L->LayerId);
		}
	}

	LM->SaveToJson();
	UE_LOG(LogPresetMgr, Log, TEXT("오브젝트 프리셋 적용: 타입=%d 위치=(%.0f, %.0f) %d 레이어"),
		(int32)Type, Center.X, Center.Y, Layers.Num());
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Enum helpers
// ─────────────────────────────────────────────────────────────────────────────

FString UPresetManager::RulesetToString(ERulesetType R)
{
	return (R == ERulesetType::Extraction) ? TEXT("extraction") : TEXT("battle_royale");
}

ERulesetType UPresetManager::StringToRuleset(const FString& Str)
{
	return Str.Equals(TEXT("extraction"), ESearchCase::IgnoreCase)
		? ERulesetType::Extraction : ERulesetType::BR;
}
