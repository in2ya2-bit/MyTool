#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "PresetTypes.h"
#include "PresetManager.generated.h"

class UDesignerIntentSubsystem;

DECLARE_MULTICAST_DELEGATE(FOnPresetsChanged);

/**
 * UPresetManager
 *
 * 레퍼런스 프리셋(내장 11종) + 커스텀 프리셋 관리.
 * 적용 시 슬라이더 5종 일괄 설정 + 적용 순서 준수.
 */
UCLASS()
class LEVELTOOL_API UPresetManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UDesignerIntentSubsystem* InSubsystem);

	// ── 프리셋 목록 ────────────────────────────────────────────────────
	const TArray<FMapPreset>& GetReferencePresets() const { return ReferencePresets; }
	const TArray<FMapPreset>& GetCustomPresets()    const { return CustomPresets; }
	TArray<FMapPreset> GetAllPresets() const;
	const FMapPreset* FindPreset(const FString& PresetName) const;

	// ── 프리셋 적용 ────────────────────────────────────────────────────
	bool ApplyPreset(const FString& PresetName);

	// ── 커스텀 프리셋 CRUD ──────────────────────────────────────────────
	bool SaveCustomPreset(const FMapPreset& Preset);
	bool DeleteCustomPreset(const FString& PresetName);
	bool ImportPreset(const FString& FilePath);
	bool ExportPreset(const FString& PresetName, const FString& DestPath) const;

	// ── 현재 슬라이더 값으로 커스텀 프리셋 생성 ─────────────────────────
	FMapPreset MakePresetFromCurrentState(const FString& Name,
	                                      const FString& Author = TEXT(""),
	                                      const FString& Desc = TEXT("")) const;

	// ── 오브젝트 프리셋 5종 (특정 위치에 구조물 패턴 배치) ──────────────
	bool ApplyObjectPreset(EObjectPresetType Type, const FVector2D& Center);

	// ── JSON I/O ────────────────────────────────────────────────────────
	void LoadCustomPresets();

	FOnPresetsChanged OnPresetsChanged;

private:
	static FString GetCustomPresetsDir();
	static FString GetPresetFilePath(const FString& PresetName);

	bool SerializePresetToJson(const FMapPreset& Preset, FString& OutJson) const;
	bool DeserializePresetFromJson(const FString& Json, FMapPreset& OutPreset) const;

	static FString RulesetToString(ERulesetType R);
	static ERulesetType StringToRuleset(const FString& Str);

	TArray<FMapPreset> ReferencePresets;
	TArray<FMapPreset> CustomPresets;

	ERulesetType LastAppliedRuleset = ERulesetType::BR;

	UPROPERTY()
	TObjectPtr<UDesignerIntentSubsystem> Subsystem;
};
