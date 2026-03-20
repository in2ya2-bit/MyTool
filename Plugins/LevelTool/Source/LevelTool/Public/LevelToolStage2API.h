#pragma once

/**
 * LevelTool Stage 2/3 API — 통합 참조 헤더
 *
 * 이 파일은 구현 코드가 아닌, 2~3단계 전체 클래스 구조를 한 눈에
 * 파악하기 위한 참조 문서입니다.
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  아키텍처 개요
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  ┌─ ULevelToolSubsystem (1단계 파이프라인) ─────────────────────────┐
 *  │  - terrain/building/road/water 생성                              │
 *  │  - map_meta.json 생성                                            │
 *  │  - stable_id 태그 부여                                           │
 *  │  └→ UEditLayerManager (소유)                                     │
 *  │       ├→ UEditLayerApplicator (Actor 반영)                       │
 *  │       └→ layers.json I/O                                         │
 *  └──────────────────────────────────────────────────────────────────┘
 *         ↓ OnStage1Complete
 *  ┌─ UDesignerIntentSubsystem (2~3단계 핵심 서브시스템) ─────────────┐
 *  │  - 슬라이더 초기값 산출 (5종)                                     │
 *  │  - ApplySlider() → 레이어 생성/교체/적용                         │
 *  │  ├→ USliderLayerGenerator (슬라이더 → FEditLayer 변환)           │
 *  │  │   - UrbanDensity, Openness, RouteComplexity                   │
 *  │  │   - ElevationContrast, DestructionLevel                       │
 *  │  ├→ UPresetManager (레퍼런스 11종 + 커스텀 프리셋)               │
 *  │  │   - ApplyPreset() → 의존성 순서대로 5개 슬라이더 적용          │
 *  │  │   - SaveCustomPreset / ImportPreset / ExportPreset            │
 *  │  └→ UChecklistEngine (3단계 체크리스트 진단)                      │
 *  │       - BR 10항목 / EX 10항목 진단                                │
 *  │       - DBSCAN 거점 탐지 (S/A/B/C 등급)                          │
 *  │       - 가중 적합도 산출 (BR/EX 가중치 합 100)                    │
 *  │       - 레퍼런스 유사도 (유클리드 거리)                           │
 *  │       - 제안 카드 생성 (AcceptSuggestion → Edit Layer 자동 생성)  │
 *  │       - Tarjan bridge edge + 4방향 접근성 분석                    │
 *  │       └→ UHeatmapGenerator (4종 히트맵)                          │
 *  │            - BuildingDensity, RoadConnectivity                    │
 *  │            - PoiDistribution, Elevation                           │
 *  │            - RenderToTexture()                                    │
 *  └──────────────────────────────────────────────────────────────────┘
 *         ↓ UI
 *  ┌─ SDesignerIntentPanel (Slate 위젯) ─────────────────────────────┐
 *  │  Tab 0: 의도 수정                                                │
 *  │   - 프리셋 버튼 (BR 5 + EX 6 + 커스텀)                          │
 *  │   - 슬라이더 5종 (드래그 → ApplySlider)                          │
 *  │   - Edit Layer 목록 (가시성 토글, 삭제)                           │
 *  │  Tab 1: 룰셋 옵션                                                │
 *  │   - 룰셋 선택 (BR/EX) + 체크 실행 버튼                           │
 *  │   - 맵 적합도 대시보드 (적합도 바 + 감점 + 유사도 Top3)          │
 *  │   - 체크리스트 결과 (10항목, 상태 아이콘 + 상세)                  │
 *  │   - 제안 카드 (수락/거절 버튼, created_by=ai_suggest)            │
 *  │   - 히트맵 토글 (4종)                                            │
 *  └──────────────────────────────────────────────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  데이터 구조 (USTRUCT)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  EditLayerTypes.h:
 *    - EEditLayerType (9종: terrain_modify ~ marker)
 *    - EEditLayerAreaType (5종: polygon/circle/point/path/actor_ref)
 *    - FEditLayerArea, FEditLayer
 *
 *  DesignerIntentTypes.h:
 *    - ESliderType (5종), ERulesetType (BR/Extraction)
 *    - FSliderState (초기/현재/한계/이유)
 *
 *  ChecklistTypes.h:
 *    - ECheckStatus (Pass/Warning/Fail/N/A)
 *    - EBRCheck (10), EEXCheck (10)
 *    - FCheckResult, FPoiCluster, FCheckReport
 *
 *  PresetTypes.h:
 *    - FPresetCoords, FSliderValues, FMapPreset
 *    - LevelToolPresets::GetBuiltInPresets() → 11종
 *
 *  SuggestionTypes.h:
 *    - ESuggestionStatus, FSuggestionCard
 *
 *  HeatmapTypes.h:
 *    - EHeatmapMode (4종), FHeatmapCell, FHeatmapData
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  파일 목록 (Sprint A0~S10)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Public/ (19 헤더):
 *    EditLayerTypes.h, EditLayerManager.h, EditLayerApplicator.h,
 *    DesignerIntentTypes.h, DesignerIntentSubsystem.h,
 *    SliderLayerGenerator.h, PresetTypes.h, PresetManager.h,
 *    ChecklistTypes.h, ChecklistEngine.h,
 *    SuggestionTypes.h, HeatmapTypes.h, HeatmapGenerator.h,
 *    SDesignerIntentPanel.h, SLevelToolPanel.h,
 *    LevelToolSubsystem.h, LevelToolModule.h, LevelToolSettings.h,
 *    LevelToolBuildingPool.h, LevelToolPerfGuard.h,
 *    LevelToolValidator.h, LevelToolStage2API.h (이 파일)
 *
 *  Private/ (17 소스):
 *    EditLayerManager.cpp, EditLayerApplicator.cpp,
 *    DesignerIntentSubsystem.cpp, SliderLayerGenerator.cpp,
 *    PresetManager.cpp, ChecklistEngine.cpp,
 *    HeatmapGenerator.cpp, SDesignerIntentPanel.cpp,
 *    LevelToolValidator.cpp,
 *    LevelToolModule.cpp, LevelToolSubsystem.cpp,
 *    LevelToolSubsystem_Landscape.cpp, LevelToolSubsystem_Buildings.cpp,
 *    LevelToolSubsystem_Roads.cpp, LevelToolSubsystem_Water.cpp,
 *    SLevelToolPanel.cpp, LevelToolSettings.cpp
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  콘솔 명령
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  LevelTool.Validate   — 전체 통합 검증 실행
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  성능 예산 (기획서 v4.4)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  진단 전체:      < 3초
 *  슬라이더 적용:  < 500ms
 *  히트맵 생성:    < 1초
 *  제안 카드:      < 200ms (최대 10건)
 *  JSON I/O:       < 100ms
 *
 *  → LevelToolPerfGuard.h의 LEVELTOOL_SCOPED_TIMER 매크로로 측정
 *    Stat LevelTool 명령으로 에디터 내 실시간 확인 가능
 */

#include "EditLayerTypes.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "DesignerIntentTypes.h"
#include "DesignerIntentSubsystem.h"
#include "SliderLayerGenerator.h"
#include "PresetTypes.h"
#include "PresetManager.h"
#include "ChecklistTypes.h"
#include "ChecklistEngine.h"
#include "SuggestionTypes.h"
#include "HeatmapTypes.h"
#include "HeatmapGenerator.h"
#include "LevelToolPerfGuard.h"
#include "LevelToolValidator.h"
