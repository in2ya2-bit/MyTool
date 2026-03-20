#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "LevelToolValidator.generated.h"

class UDesignerIntentSubsystem;
class UEditLayerManager;
class UChecklistEngine;

/**
 * ULevelToolValidator
 *
 * 2~3단계 통합 검증 유틸리티.
 * 파이프라인 전 구간의 정합성을 자동 검사하고 결과를 로그에 출력한다.
 * 에디터 콘솔에서 `LevelTool.Validate` 명령으로도 호출 가능.
 */
UCLASS()
class LEVELTOOL_API ULevelToolValidator : public UObject
{
	GENERATED_BODY()

public:
	struct FValidationResult
	{
		int32 PassCount  = 0;
		int32 WarnCount  = 0;
		int32 ErrorCount = 0;
		TArray<FString> Messages;
		bool IsAllPass() const { return ErrorCount == 0 && WarnCount == 0; }
	};

	static FValidationResult RunFullValidation();

private:
	static void ValidateMapMeta(FValidationResult& Out);
	static void ValidateLayersJson(FValidationResult& Out);
	static void ValidateEditLayerManager(FValidationResult& Out);
	static void ValidateDesignerIntent(FValidationResult& Out);
	static void ValidateChecklistEngine(FValidationResult& Out);
	static void ValidatePresetManager(FValidationResult& Out);
	static void ValidateStableIds(FValidationResult& Out);

	static void Pass(FValidationResult& Out, const FString& Msg);
	static void Warn(FValidationResult& Out, const FString& Msg);
	static void Error(FValidationResult& Out, const FString& Msg);
};
