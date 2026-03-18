#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "LevelToolSubsystem.h"
#include "LevelToolBuildingPool.h"

/**
 * SLevelToolPanel
 *
 * Main Slate widget shown in the "Level Tool" editor tab.
 * Layout:
 *   ┌──────────────────────────────────────────────────────┐
 *   │  ▌ Location                                          │
 *   │  [Preset dropdown ▼]  or  [Lat] [Lon] [Radius]      │
 *   │                                                       │
 *   │  ▌ Options                                           │
 *   │  [●] Generate Landscape   [●] Place Buildings        │
 *   │  Source [▼] Elevation source                         │
 *   │  Building Pool [▲ DA_BuildingPool]                   │
 *   │                                                       │
 *   │  [ ▶ Generate Map ]   [ ✕ Cancel ]                   │
 *   │  ░░░░░░░░░░░░░░░░░░░░░ 64%  Placing buildings...    │
 *   │                                                       │
 *   │  ▌ Log                                               │
 *   │  ┌──────────────────────────────────────────────┐   │
 *   │  │ [07:32] ✔ Elevation fetched: 1009×1009       │   │
 *   │  │ [07:33] ✔ Landscape imported                 │   │
 *   │  └──────────────────────────────────────────────┘   │
 *   └──────────────────────────────────────────────────────┘
 */
class LEVELTOOL_API SLevelToolPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLevelToolPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SLevelToolPanel();

private:
    // ── UI Build ─────────────────────────────────────────────────────────
    TSharedRef<SWidget> BuildLocationSection();
    TSharedRef<SWidget> BuildOptionsSection();
    TSharedRef<SWidget> BuildActionBar();
    TSharedRef<SWidget> BuildProgressBar();
    TSharedRef<SWidget> BuildLogSection();

    // ── Section header helper ─────────────────────────────────────────
    TSharedRef<SWidget> MakeSectionHeader(const FText& Label);

    // ── Preset Dropdown ──────────────────────────────────────────────
    TSharedRef<SWidget> MakePresetComboContent();
    void                OnPresetSelected(TSharedPtr<FString> Item, ESelectInfo::Type);
    FText               GetSelectedPresetText() const;

    // ── Coordinate inputs ────────────────────────────────────────────
    float               GetLatValue()    const;
    float               GetLonValue()    const;
    float               GetRadiusValue() const;
    void                OnLatChanged(float Val);
    void                OnLonChanged(float Val);
    void                OnRadiusChanged(float Val);
    bool                IsCustomCoordsEnabled() const;

    // ── Elevation source combo ───────────────────────────────────────
    TSharedRef<SWidget> MakeElevationSourceRow();
    TSharedRef<SWidget> MakeElevSourceComboContent();
    void                OnElevSourceSelected(TSharedPtr<FString> Item, ESelectInfo::Type);
    FText               GetElevSourceText() const;

    // ── Building pool picker ─────────────────────────────────────────
    FString             GetBuildingPoolPath() const;
    void                OnBuildingPoolChanged(const FAssetData& AssetData);

    // ── Action buttons ───────────────────────────────────────────────
    FReply              OnGenerateClicked();
    FReply              OnCancelClicked();
    FReply              OnClearClicked();
    bool                IsGenerateEnabled() const;
    bool                IsCancelEnabled()   const;

    // ── Progress bar ─────────────────────────────────────────────────
    TOptional<float>    GetProgressPercent()    const;
    FText               GetProgressStageText() const;
    FSlateColor         GetProgressBarColor()   const;

    // ── Preview ──────────────────────────────────────────────────────
    TSharedRef<SWidget> BuildPreviewSection();
    void                LoadPreviewTexture(const FString& PngPath);

    // ── Log ──────────────────────────────────────────────────────────
    TSharedRef<ITableRow> GenerateLogRow(TSharedPtr<FString> Item,
                                          const TSharedRef<STableViewBase>& OwnerTable);
    void                ScrollLogToBottom();
    FReply              OnClearLogClicked();
    FReply              OnCopyLogClicked();

    // ── Subsystem event handlers ─────────────────────────────────────
    void OnProgress(FString Stage, float Percent);
    void OnComplete(bool bSuccess);
    void OnLogLine(FString Line);

    void RefreshPresetList();

    // ── State ────────────────────────────────────────────────────────
    TWeakObjectPtr<ULevelToolSubsystem> Subsystem;

    // Location
    TArray<TSharedPtr<FString>> PresetItems;
    TSharedPtr<FString>         SelectedPreset;
    float CurrentLat    = 37.5704f;
    float CurrentLon    = 126.9820f;
    float CurrentRadius = 1.0f;
    bool  bUsePreset    = true;

    // Options
    bool  bDoLandscape  = true;
    bool  bDoBuildings  = true;
    bool  bDoRoads      = true;
    TArray<TSharedPtr<FString>>  ElevSourceItems;
    TSharedPtr<FString>          SelectedElevSource;
    TWeakObjectPtr<ULevelToolBuildingPool> BuildingPool;

    // Progress
    float  CurrentProgress    = 0.0f;
    FString CurrentStage      = TEXT("Idle");
    bool   bLastJobSucceeded  = true;
    bool   bJobFinished       = false;

    // Log
    TArray<TSharedPtr<FString>>        LogEntries;
    TSharedPtr<SListView<TSharedPtr<FString>>> LogListView;

    // Delegate handles (for cleanup)
    FDelegateHandle ProgressHandle;
    FDelegateHandle CompleteHandle;
    FDelegateHandle LogHandle;

    // Combo boxes
    TSharedPtr<SComboBox<TSharedPtr<FString>>> PresetCombo;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ElevSourceCombo;

    // Preview
    TWeakObjectPtr<UTexture2D> PreviewTexture;
    TSharedPtr<FSlateBrush>    PreviewBrush;
    TSharedPtr<SImage>         PreviewImage;
};
