#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "DesignerIntentTypes.h"
#include "HeatmapTypes.h"

class UDesignerIntentSubsystem;
class UEditLayerManager;
class UPresetManager;
class SWidgetSwitcher;
class SVerticalBox;
struct FEditLayer;
struct FMapPreset;
struct FCheckReport;
struct FCheckResult;

/**
 * SDesignerIntentPanel
 *
 * 2~3단계 Slate 위젯.  "의도 수정" 탭 + "룰셋 옵션" 탭.
 *
 *   ┌────────────────────────────────────────────────────────┐
 *   │ [Tab: 의도 수정]   [Tab: 룰셋 옵션]                    │
 *   ├────────────────────────────────────────────────────────┤
 *   │  슬라이더 (5종)  ·  프리셋  ·  Edit Layer 목록         │
 *   └────────────────────────────────────────────────────────┘
 */
class LEVELTOOL_API SDesignerIntentPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDesignerIntentPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDesignerIntentPanel();

private:
	// ── UI Build ─────────────────────────────────────────────────────────
	TSharedRef<SWidget> BuildTabBar();
	TSharedRef<SWidget> BuildIntentTab();
	TSharedRef<SWidget> BuildRulesetTab();

	// 의도 수정 탭 하위 섹션
	TSharedRef<SWidget> BuildPresetSection();
	TSharedRef<SWidget> BuildSliderSection();
	TSharedRef<SWidget> BuildSliderRow(ESliderType Type, const FText& Label);
	TSharedRef<SWidget> BuildLayerListSection();
	TSharedRef<SWidget> BuildSelectionModeBar();

	EPresetSelectionMode CurrentSelectionMode = EPresetSelectionMode::None;

	// 룰셋 옵션 탭 하위 섹션
	TSharedRef<SWidget> BuildRulesetSelector();
	TSharedRef<SWidget> BuildChecklistSection();
	TSharedRef<SWidget> BuildSuggestionSection();
	TSharedRef<SWidget> BuildDashboardSection();
	TSharedRef<SWidget> BuildHeatmapToggleSection();

	// 공통 헬퍼
	TSharedRef<SWidget> MakeSectionHeader(const FText& Label);

	// ── Tab switching ────────────────────────────────────────────────────
	FReply OnTabClicked(int32 TabIndex);
	FSlateColor GetTabColor(int32 TabIndex) const;

	// ── Slider callbacks ─────────────────────────────────────────────────
	float GetSliderValue(ESliderType Type) const;
	void  OnSliderChanged(float NewValue, ESliderType Type);
	void  OnSliderCommitted(float NewValue, ESliderType Type);
	FText GetSliderValueText(ESliderType Type) const;
	FText GetSliderInitialText(ESliderType Type) const;

	// ── P2-14: 변경 프리뷰 (적합도 시뮬레이션) ───────────────────────
	float SimulateFitnessChange(ESliderType Type, float NewValue) const;
	TSharedPtr<STextBlock> FitnessPreviewText;

	// ── Layer list ───────────────────────────────────────────────────────
	TSharedRef<ITableRow> GenerateLayerRow(
		TSharedPtr<FString> LayerId,
		const TSharedRef<STableViewBase>& OwnerTable);
	void RefreshLayerList();
	FReply OnToggleLayerVisible(FString LayerId);
	FReply OnDeleteLayer(FString LayerId);

	// ── P1-11: Layer grouping / folding ─────────────────────────────────
	FReply OnToggleGroupFold(FString GroupKey);
	FString GetGroupKey(const FEditLayer& Layer) const;

	// ── G-2: 레이어 필터 ────────────────────────────────────────────────
	TSharedRef<SWidget> BuildLayerFilterBar();
	FReply OnToggleFilter(FString FilterKey);
	TSet<FString> ActiveFilters;

	// ── G-4: 그룹별 숨기기/삭제 ─────────────────────────────────────────
	FReply OnHideGroup(FString GroupKey);
	FReply OnDeleteGroup(FString GroupKey);

	// ── Preset callbacks ─────────────────────────────────────────────────
	FReply OnPresetClicked(FString PresetName);
	FReply OnSaveCustomPreset();

	// ── Ruleset ──────────────────────────────────────────────────────────
	FReply OnRunDiagnosis();
	void   RebuildChecklistResults();
	TSharedRef<SWidget> BuildCheckResultRow(const FCheckResult& Result);
	FLinearColor StatusColor(uint8 Status) const;
	FText        StatusIcon(uint8 Status) const;

	// ── Suggestions ─────────────────────────────────────────────────────
	FReply OnFocusSuggestion(FString CardId);
	FReply OnAcceptSuggestion(FString CardId);
	FReply OnRejectSuggestion(FString CardId);
	void   RebuildSuggestionCards();

	// ── Suggestion focus ring ───────────────────────────────────────────
	void ShowFocusRing(const FVector& Location);
	void ClearFocusRing();
	TWeakObjectPtr<AActor> FocusRingActor;

	// ── Dashboard ───────────────────────────────────────────────────────
	void   RebuildDashboard();

	// ── Heatmap overlay ─────────────────────────────────────────────────
	FReply OnToggleHeatmap(EHeatmapMode Mode);
	FReply OnHideHeatmap();

	// ── P2-12: Viewport focus ───────────────────────────────────────────
	static void FocusViewportOn(const FVector& Location, float Distance = 5000.f);

	// ── P2-13: Hover highlight ──────────────────────────────────────────
	void HighlightActors(const TArray<FString>& StableIds);
	void ClearHighlight();
	TArray<TWeakObjectPtr<AActor>> HighlightedActors;

	// ── P1-5: 뷰포트 프리뷰 (CustomDepth) ──────────────────────────────
	void SetActorsCustomDepth(const TArray<AActor*>& Actors, bool bEnable, int32 StencilValue = 0);
	TArray<TWeakObjectPtr<AActor>> PreviewHighlightedActors;

	// ── P2-11: 교차 레이어 Warning ──────────────────────────────────────
	TMultiMap<FString, FString> ConflictWarnings;

	// ── Subsystem event handlers ─────────────────────────────────────────
	void OnSliderInitialized(const FString& MapId);
	void OnLayersChanged();

	// ── State ────────────────────────────────────────────────────────────
	TWeakObjectPtr<UDesignerIntentSubsystem> IntentSubsystem;
	TWeakObjectPtr<UEditLayerManager>        LayerManager;

	int32 ActiveTabIndex = 0;
	TSharedPtr<SWidgetSwitcher> TabSwitcher;

	// 슬라이더 현재값 (드래그 중 임시 저장)
	TMap<ESliderType, float> SliderValues;

	// 레이어 목록
	TArray<TSharedPtr<FString>>                      LayerIds;
	TSharedPtr<SListView<TSharedPtr<FString>>>       LayerListView;

	// P1-11 그룹 폴딩 상태 (GroupKey → folded)
	TSet<FString> FoldedGroups;

	// 룰셋 선택 & 진단 결과
	int32 SelectedRuleset = 0;   // 0=BR, 1=Extraction
	TSharedPtr<SVerticalBox> CheckResultBox;
	TSharedPtr<SVerticalBox> SuggestionBox;
	TSharedPtr<SVerticalBox> DashboardBox;
	TSharedPtr<STextBlock>   ScoreText;

	// 델리게이트 핸들
	FDelegateHandle SliderInitHandle;
	FDelegateHandle LayerChangeHandle;
};
