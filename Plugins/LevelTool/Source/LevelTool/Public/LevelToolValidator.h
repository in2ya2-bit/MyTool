#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "LevelToolValidator.generated.h"

class UEditLayerManager;

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
	static void ValidateStableIds(FValidationResult& Out);

	static void Pass(FValidationResult& Out, const FString& Msg);
	static void Warn(FValidationResult& Out, const FString& Msg);
	static void Error(FValidationResult& Out, const FString& Msg);
};
