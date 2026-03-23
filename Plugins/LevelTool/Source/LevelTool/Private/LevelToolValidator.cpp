#include "LevelToolValidator.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"
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
	TEXT("Run LevelTool integration validation"),
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
	auto* LTS = GEditor ? GEditor->GetEditorSubsystem<ULevelToolSubsystem>() : nullptr;
	auto* LM = LTS ? LTS->GetEditLayerManager() : nullptr;
	if (!LM)
	{
		Warn(Out, TEXT("EditLayerManager 없음 — 에디터 모드 확인 필요"));
		return;
	}

	const FString MapId = LM->GetMapId();
	if (MapId.IsEmpty())
	{
		Warn(Out, TEXT("MapId 비어있음 — 1단계 미완료"));
		return;
	}
	Pass(Out, FString::Printf(TEXT("MapId: %s"), *MapId));

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
