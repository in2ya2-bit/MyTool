#pragma once

#include "CoreMinimal.h"
#include "EditLayerTypes.h"
#include "SuggestionTypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
//  제안 카드 상태
// ─────────────────────────────────────────────────────────────────────────────

UENUM()
enum class ESuggestionStatus : uint8
{
	Pending,     // 미결정
	Accepted,    // 수락됨 → Edit Layer 생성됨
	Rejected,    // 거절됨
	Skipped      // 해당 없음 (선택 사항)
};

// ─────────────────────────────────────────────────────────────────────────────
//  제안 카드
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT()
struct FSuggestionCard
{
	GENERATED_BODY()

	FString  CardId;         // "suggest_{check_id}_{index}"
	FString  CheckId;        // "BR-3", "EX-8" 등
	FString  Problem;        // "서쪽 다리를 차단하면 Military Base 영역이 고립됩니다"
	FString  Reference;      // "Erangel: 동서 다리 + 캣워크로 병목 우회 보장"

	// 수락 시 자동 생성할 Edit Layer
	FEditLayer SuggestedLayer;

	ESuggestionStatus Status = ESuggestionStatus::Pending;
	FString CreatedLayerId;  // 수락 시 생성된 LayerId

	FVector  FocusLocation = FVector::ZeroVector;

	FString GetStatusText() const
	{
		switch (Status)
		{
		case ESuggestionStatus::Accepted: return TEXT("수락됨");
		case ESuggestionStatus::Rejected: return TEXT("거절됨");
		case ESuggestionStatus::Skipped:  return TEXT("건너뜀");
		default:                           return TEXT("미결정");
		}
	}
};
