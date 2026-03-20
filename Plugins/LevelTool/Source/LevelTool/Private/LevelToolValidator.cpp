#include "LevelToolValidator.h"
#include "DesignerIntentSubsystem.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"
#include "ChecklistEngine.h"
#include "ChecklistTypes.h"
#include "PresetManager.h"
#include "PresetTypes.h"
#include "HeatmapGenerator.h"
#include "LevelToolSubsystem.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "EngineUtils.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogLTValidator, Log, All);

static FAutoConsoleCommand CmdValidate(
	TEXT("LevelTool.Validate"),
	TEXT("Run full LevelTool Stage 2/3 integration validation"),
	FConsoleCommandDelegate::CreateLambda([]
	{
		ULevelToolValidator::RunFullValidation();
	}));

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::Pass(FValidationResult& Out, const FString& Msg)
{
	Out.PassCount++;
	Out.Messages.Add(FString::Printf(TEXT("[PASS] %s"), *Msg));
	UE_LOG(LogLTValidator, Log, TEXT("[PASS] %s"), *Msg);
}

void ULevelToolValidator::Warn(FValidationResult& Out, const FString& Msg)
{
	Out.WarnCount++;
	Out.Messages.Add(FString::Printf(TEXT("[WARN] %s"), *Msg));
	UE_LOG(LogLTValidator, Warning, TEXT("[WARN] %s"), *Msg);
}

void ULevelToolValidator::Error(FValidationResult& Out, const FString& Msg)
{
	Out.ErrorCount++;
	Out.Messages.Add(FString::Printf(TEXT("[ERROR] %s"), *Msg));
	UE_LOG(LogLTValidator, Error, TEXT("[ERROR] %s"), *Msg);
}

// ─────────────────────────────────────────────────────────────────────────────
ULevelToolValidator::FValidationResult ULevelToolValidator::RunFullValidation()
{
	FValidationResult Out;

	UE_LOG(LogLTValidator, Log, TEXT("═══════════════════════════════════════════════════"));
	UE_LOG(LogLTValidator, Log, TEXT("  LevelTool 통합 검증 시작"));
	UE_LOG(LogLTValidator, Log, TEXT("═══════════════════════════════════════════════════"));

	ValidateMapMeta(Out);
	ValidateLayersJson(Out);
	ValidateEditLayerManager(Out);
	ValidateDesignerIntent(Out);
	ValidateChecklistEngine(Out);
	ValidatePresetManager(Out);
	ValidateStableIds(Out);

	UE_LOG(LogLTValidator, Log, TEXT("═══════════════════════════════════════════════════"));
	UE_LOG(LogLTValidator, Log, TEXT("  결과: Pass=%d  Warn=%d  Error=%d  %s"),
		Out.PassCount, Out.WarnCount, Out.ErrorCount,
		Out.IsAllPass() ? TEXT("ALL PASS") : TEXT("ISSUES FOUND"));
	UE_LOG(LogLTValidator, Log, TEXT("═══════════════════════════════════════════════════"));

	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateMapMeta(FValidationResult& Out)
{
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	if (!DIS)
	{
		Warn(Out, TEXT("DesignerIntentSubsystem 없음 — 에디터 모드 확인 필요"));
		return;
	}

	const FString MapId = DIS->GetActiveMapId();
	if (MapId.IsEmpty())
	{
		Warn(Out, TEXT("ActiveMapId 비어있음 — 1단계 미완료"));
		return;
	}
	Pass(Out, FString::Printf(TEXT("ActiveMapId: %s"), *MapId));

	const FString MetaPath = FPaths::ProjectSavedDir() / TEXT("LevelTool") / TEXT("EditLayers")
		/ MapId / TEXT("map_meta.json");

	if (!FPaths::FileExists(MetaPath))
	{
		Error(Out, FString::Printf(TEXT("map_meta.json 없음: %s"), *MetaPath));
		return;
	}
	Pass(Out, TEXT("map_meta.json 존재"));

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *MetaPath))
	{
		Error(Out, TEXT("map_meta.json 읽기 실패"));
		return;
	}

	TSharedPtr<FJsonObject> Root;
	auto Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		Error(Out, TEXT("map_meta.json JSON 파싱 실패"));
		return;
	}

	if (Root->HasField(TEXT("origin")))            Pass(Out, TEXT("map_meta: origin 필드 존재"));
	else                                            Error(Out, TEXT("map_meta: origin 필드 누락"));

	if (Root->HasField(TEXT("terrain_profile")))    Pass(Out, TEXT("map_meta: terrain_profile 존재"));
	else                                            Warn(Out, TEXT("map_meta: terrain_profile 누락"));

	if (Root->HasField(TEXT("slider_initial_values"))) Pass(Out, TEXT("map_meta: slider_initial_values 존재"));
	else                                                Warn(Out, TEXT("map_meta: slider_initial_values 미생성 — 슬라이더 초기값 미산출"));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateLayersJson(FValidationResult& Out)
{
	auto* LTS = GEditor ? GEditor->GetEditorSubsystem<ULevelToolSubsystem>() : nullptr;
	auto* LM = LTS ? LTS->GetEditLayerManager() : nullptr;
	if (!LM)
	{
		Warn(Out, TEXT("EditLayerManager 미초기화"));
		return;
	}
	Pass(Out, TEXT("EditLayerManager 활성"));

	const TArray<FEditLayer>& Layers = LM->GetAllLayers();
	Pass(Out, FString::Printf(TEXT("등록된 Edit Layer: %d개"), Layers.Num()));

	int32 InvalidCount = 0;
	for (const FEditLayer& L : Layers)
	{
		if (L.LayerId.IsEmpty()) InvalidCount++;
	}
	if (InvalidCount > 0)
		Error(Out, FString::Printf(TEXT("LayerId 비어있는 레이어 %d개"), InvalidCount));
	else
		Pass(Out, TEXT("모든 레이어 LayerId 유효"));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateEditLayerManager(FValidationResult& Out)
{
	auto* LTS = GEditor ? GEditor->GetEditorSubsystem<ULevelToolSubsystem>() : nullptr;
	auto* LM = LTS ? LTS->GetEditLayerManager() : nullptr;
	if (!LM) return;

	if (LM->GetApplicator())
		Pass(Out, TEXT("EditLayerApplicator 활성"));
	else
		Error(Out, TEXT("EditLayerApplicator nullptr"));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateDesignerIntent(FValidationResult& Out)
{
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	if (!DIS)
	{
		Warn(Out, TEXT("DesignerIntentSubsystem 없음"));
		return;
	}
	Pass(Out, TEXT("DesignerIntentSubsystem 활성"));

	const auto& States = DIS->GetAllSliderStates();
	if (States.Num() == 5)
		Pass(Out, TEXT("슬라이더 5종 등록 완료"));
	else
		Error(Out, FString::Printf(TEXT("슬라이더 %d종 (기대: 5)"), States.Num()));

	for (const auto& KV : States)
	{
		if (KV.Value.CurrentValue < 0.f || KV.Value.CurrentValue > 100.f)
			Warn(Out, FString::Printf(TEXT("슬라이더 %d 범위 초과: %.1f"), (int32)KV.Key, KV.Value.CurrentValue));
	}

	if (DIS->GetPresetManager())
		Pass(Out, TEXT("PresetManager 활성"));
	else
		Error(Out, TEXT("PresetManager nullptr"));

	if (DIS->GetChecklistEngine())
		Pass(Out, TEXT("ChecklistEngine 활성"));
	else
		Error(Out, TEXT("ChecklistEngine nullptr"));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateChecklistEngine(FValidationResult& Out)
{
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	auto* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (!Engine) return;

	if (Engine->GetHeatmapGenerator())
		Pass(Out, TEXT("HeatmapGenerator 활성"));
	else
		Error(Out, TEXT("HeatmapGenerator nullptr"));

	const FCheckReport& Report = Engine->GetLastReport();
	if (Report.Results.Num() > 0)
		Pass(Out, FString::Printf(TEXT("마지막 진단: %d 항목, 적합도 %.0f"),
			Report.Results.Num(), Report.TotalScore));
	else
		Warn(Out, TEXT("진단 미실행 (체크리스트 비어있음)"));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidatePresetManager(FValidationResult& Out)
{
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	auto* PM = DIS ? DIS->GetPresetManager() : nullptr;
	if (!PM) return;

	if (PM->GetReferencePresets().Num() == 11)
		Pass(Out, TEXT("레퍼런스 프리셋 11종 로드"));
	else
		Error(Out, FString::Printf(TEXT("레퍼런스 프리셋 %d종 (기대: 11)"), PM->GetReferencePresets().Num()));

	Pass(Out, FString::Printf(TEXT("커스텀 프리셋 %d종"), PM->GetCustomPresets().Num()));
}

// ─────────────────────────────────────────────────────────────────────────────
void ULevelToolValidator::ValidateStableIds(FValidationResult& Out)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Warn(Out, TEXT("EditorWorld 없음 — StableID 검증 생략"));
		return;
	}

	int32 TaggedCount = 0;
	int32 GeneratedCount = 0;
	int32 DuplicateCount = 0;
	TSet<FString> SeenIds;

	static const FName kMarkerTag(TEXT("LevelTool_StableID"));
	static const FName kGeneratedTag(TEXT("LevelTool_Generated"));

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		bool bHasMarker = Actor->Tags.Contains(kMarkerTag);
		bool bHasGenerated = Actor->Tags.Contains(kGeneratedTag);

		if (bHasGenerated) GeneratedCount++;

		if (bHasMarker)
		{
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag == kMarkerTag || Tag == kGeneratedTag) continue;
				FString IdStr = Tag.ToString();
				if (IdStr.StartsWith(TEXT("bldg_")) || IdStr.StartsWith(TEXT("road_"))
					|| IdStr.StartsWith(TEXT("marker_")) || IdStr.StartsWith(TEXT("roadblock_")))
				{
					TaggedCount++;
					if (SeenIds.Contains(IdStr))
						DuplicateCount++;
					else
						SeenIds.Add(IdStr);
				}
			}
		}
	}

	Pass(Out, FString::Printf(TEXT("StableID 태그 Actor: %d개"), TaggedCount));
	Pass(Out, FString::Printf(TEXT("Generated 태그 Actor: %d개"), GeneratedCount));

	if (DuplicateCount > 0)
		Error(Out, FString::Printf(TEXT("중복 StableID: %d건"), DuplicateCount));
	else
		Pass(Out, TEXT("StableID 중복 없음"));
}
