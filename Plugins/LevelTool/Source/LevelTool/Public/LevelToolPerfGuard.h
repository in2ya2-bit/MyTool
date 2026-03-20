#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

/**
 * LevelTool 성능 프로파일링 매크로 및 가드.
 *
 * UE5 Stat 시스템에 LevelTool 전용 그룹을 등록하고,
 * 주요 연산에 SCOPE_TIMER 매크로를 제공한다.
 *
 * 사용법:
 *   LEVELTOOL_SCOPED_TIMER(SliderApply);
 *   LEVELTOOL_SCOPED_TIMER(ChecklistDiagnosis);
 */

DECLARE_STATS_GROUP(TEXT("LevelTool"), STATGROUP_LevelTool, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("SliderApply"),       STAT_LT_SliderApply,       STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("LayerApply"),         STAT_LT_LayerApply,        STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("LayerHide"),          STAT_LT_LayerHide,         STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("CheckDiagnosis"),     STAT_LT_CheckDiagnosis,    STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("DBSCAN"),             STAT_LT_DBSCAN,            STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("HeatmapGenerate"),    STAT_LT_HeatmapGenerate,   STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("PresetApply"),        STAT_LT_PresetApply,       STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("BridgeEdgeDetect"),   STAT_LT_BridgeEdge,        STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("SuggestionGenerate"), STAT_LT_SuggestionGen,     STATGROUP_LevelTool);
DECLARE_CYCLE_STAT(TEXT("LayerJsonIO"),        STAT_LT_LayerJsonIO,       STATGROUP_LevelTool);

#define LEVELTOOL_SCOPED_TIMER(StatName) SCOPE_CYCLE_COUNTER(STAT_LT_##StatName)

/**
 * 대형 맵 성능 예산 상수.
 * 기획서 v4.4 성능 목표 기반.
 */
namespace LevelToolPerf
{
	inline constexpr double kMaxDiagnosisMs      = 3000.0;   // 전체 진단 < 3초
	inline constexpr double kMaxSliderApplyMs     = 500.0;    // 슬라이더 적용 < 500ms
	inline constexpr double kMaxHeatmapMs         = 1000.0;   // 히트맵 생성 < 1초
	inline constexpr double kMaxSuggestionMs      = 200.0;    // 제안 카드 생성 < 200ms
	inline constexpr double kMaxLayerJsonIOMs     = 100.0;    // JSON I/O < 100ms
	inline constexpr int32  kMaxSuggestionCards   = 10;
	inline constexpr int32  kDBSCAN_MaxBuildings  = 50000;    // 50k 건물까지 지원

	inline float SampleIntervalCm(float MapRadiusKm)
	{
		return FMath::Max(1000.f, MapRadiusKm * 1000.f);  // max(10m, radius_m/100)
	}

	inline float DouglasPeuckerEpsilon(float MapRadiusKm)
	{
		return SampleIntervalCm(MapRadiusKm);
	}
}
