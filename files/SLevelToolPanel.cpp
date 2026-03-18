#include "SLevelToolPanel.h"
#include "LevelToolSettings.h"

#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Notifications/SProgressBar.h"

#include "PropertyCustomizationHelpers.h"   // SObjectPropertyEntryBox
#include "EditorStyleSet.h"
#include "Styling/AppStyle.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "LevelTool"

// Shared style constants
static const FLinearColor kAccentBlue  (0.20f, 0.55f, 1.00f);
static const FLinearColor kAccentGreen (0.20f, 0.80f, 0.40f);
static const FLinearColor kAccentRed   (1.00f, 0.35f, 0.30f);
static const FLinearColor kAccentGray  (0.45f, 0.45f, 0.45f);
static const FMargin      kSectionPad  (12.f, 10.f, 12.f, 6.f);
static const FMargin      kRowPad      (4.f, 3.f);
static const float        kLabelWidth  = 120.f;
static const float        kInputWidth  = 200.f;
static const float        kBtnHeight   = 32.f;

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────

void SLevelToolPanel::Construct(const FArguments& InArgs)
{
    // Get subsystem
    Subsystem = GEditor->GetEditorSubsystem<ULevelToolSubsystem>();

    // Wire events
    if (ULevelToolSubsystem* Sub = Subsystem.Get())
    {
        ProgressHandle = Sub->OnProgress.AddSP(this, &SLevelToolPanel::OnProgress);
        CompleteHandle = Sub->OnComplete.AddSP(this, &SLevelToolPanel::OnComplete);
        LogHandle      = Sub->OnLog     .AddSP(this, &SLevelToolPanel::OnLogLine);
    }

    // Elevation source list
    ElevSourceItems.Add(MakeShared<FString>(TEXT("Open-Elevation (Free, SRTM)")));
    ElevSourceItems.Add(MakeShared<FString>(TEXT("OpenTopography (Free key)")));
    ElevSourceItems.Add(MakeShared<FString>(TEXT("Google Maps (Paid)")));
    SelectedElevSource = ElevSourceItems[0];

    RefreshPresetList();

    // Build UI
    ChildSlot
    [
        SNew(SScrollBox)
        + SScrollBox::Slot()
        [
            SNew(SVerticalBox)

            // ─── Title bar ───────────────────────────────────────────
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(12.f, 12.f, 12.f, 4.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("Title", "Level Tool"))
                    .TextStyle(FAppStyle::Get(), "NormalText.Important")
                    .ColorAndOpacity(FLinearColor::White)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("Subtitle", "Real-world map generator"))
                    .ColorAndOpacity(kAccentGray)
                ]
            ]

            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SSeparator).Thickness(1.f) ]

            // ─── Location ────────────────────────────────────────────
            + SVerticalBox::Slot().AutoHeight()
            [ BuildLocationSection() ]

            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kAccentGray * 0.5f) ]

            // ─── Options ─────────────────────────────────────────────
            + SVerticalBox::Slot().AutoHeight()
            [ BuildOptionsSection() ]

            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kAccentGray * 0.5f) ]

            // ─── Action bar ──────────────────────────────────────────
            + SVerticalBox::Slot().AutoHeight()
            [ BuildActionBar() ]

            // ─── Progress ────────────────────────────────────────────
            + SVerticalBox::Slot().AutoHeight().Padding(12.f, 6.f)
            [ BuildProgressBar() ]

            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kAccentGray * 0.5f) ]

            // ─── Log ─────────────────────────────────────────────────
            + SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 0.f)
            [ BuildLogSection() ]
        ]
    ];
}

SLevelToolPanel::~SLevelToolPanel()
{
    if (ULevelToolSubsystem* Sub = Subsystem.Get())
    {
        Sub->OnProgress.Remove(ProgressHandle);
        Sub->OnComplete.Remove(CompleteHandle);
        Sub->OnLog     .Remove(LogHandle);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section: Location
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SLevelToolPanel::BuildLocationSection()
{
    return SNew(SVerticalBox)
    .Clipping(EWidgetClipping::ClipToBounds)

    + SVerticalBox::Slot().AutoHeight().Padding(kSectionPad)
    [ MakeSectionHeader(LOCTEXT("LocationHeader", "Location")) ]

    // Use preset toggle
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 2.f)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]{ return bUsePreset ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State){
                bUsePreset = (State == ECheckBoxState::Checked);
            })
        ]
        + SHorizontalBox::Slot().VAlign(VAlign_Center)
        [
            SNew(STextBlock).Text(LOCTEXT("UsePreset", "Use preset location"))
            .ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f))
        ]
    ]

    // Preset combo row
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
    [
        SNew(SHorizontalBox)
        .Visibility_Lambda([this]{ return bUsePreset ? EVisibility::Visible : EVisibility::Collapsed; })

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        .Padding(0,0,8,0)
        [
            SNew(SBox).WidthOverride(kLabelWidth)
            [ SNew(STextBlock).Text(LOCTEXT("PresetLabel","Preset"))
              .ColorAndOpacity(kAccentGray) ]
        ]
        + SHorizontalBox::Slot()
        [
            SAssignNew(PresetCombo, SComboBox<TSharedPtr<FString>>)
            .OptionsSource(&PresetItems)
            .OnSelectionChanged(this, &SLevelToolPanel::OnPresetSelected)
            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
                return SNew(STextBlock).Text(FText::FromString(*Item));
            })
            [ SNew(STextBlock).Text(this, &SLevelToolPanel::GetSelectedPresetText) ]
        ]
    ]

    // Custom coordinate rows
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
    [
        SNew(SVerticalBox)
        .Visibility_Lambda([this]{ return !bUsePreset ? EVisibility::Visible : EVisibility::Collapsed; })

        // Latitude
        + SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
            [
                SNew(SBox).WidthOverride(kLabelWidth)
                [ SNew(STextBlock).Text(LOCTEXT("LatLabel","Latitude"))
                  .ColorAndOpacity(kAccentGray) ]
            ]
            + SHorizontalBox::Slot()
            [
                SNew(SBox).WidthOverride(kInputWidth)
                [
                    SNew(SSpinBox<float>)
                    .MinValue(-90.f).MaxValue(90.f)
                    .Value(this, &SLevelToolPanel::GetLatValue)
                    .OnValueChanged(this, &SLevelToolPanel::OnLatChanged)
                    .Delta(0.001f).MinFractionalDigits(4).MaxFractionalDigits(6)
                ]
            ]
        ]

        // Longitude
        + SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
            [
                SNew(SBox).WidthOverride(kLabelWidth)
                [ SNew(STextBlock).Text(LOCTEXT("LonLabel","Longitude"))
                  .ColorAndOpacity(kAccentGray) ]
            ]
            + SHorizontalBox::Slot()
            [
                SNew(SBox).WidthOverride(kInputWidth)
                [
                    SNew(SSpinBox<float>)
                    .MinValue(-180.f).MaxValue(180.f)
                    .Value(this, &SLevelToolPanel::GetLonValue)
                    .OnValueChanged(this, &SLevelToolPanel::OnLonChanged)
                    .Delta(0.001f).MinFractionalDigits(4).MaxFractionalDigits(6)
                ]
            ]
        ]

        // Radius
        + SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
            [
                SNew(SBox).WidthOverride(kLabelWidth)
                [ SNew(STextBlock).Text(LOCTEXT("RadiusLabel","Radius (km)"))
                  .ColorAndOpacity(kAccentGray) ]
            ]
            + SHorizontalBox::Slot()
            [
                SNew(SBox).WidthOverride(kInputWidth)
                [
                    SNew(SSpinBox<float>)
                    .MinValue(0.1f).MaxValue(10.f)
                    .Value(this, &SLevelToolPanel::GetRadiusValue)
                    .OnValueChanged(this, &SLevelToolPanel::OnRadiusChanged)
                    .Delta(0.1f).MinFractionalDigits(1).MaxFractionalDigits(2)
                ]
            ]
        ]
    ];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section: Options
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SLevelToolPanel::BuildOptionsSection()
{
    return SNew(SVerticalBox)

    + SVerticalBox::Slot().AutoHeight().Padding(kSectionPad)
    [ MakeSectionHeader(LOCTEXT("OptionsHeader", "Generation options")) ]

    // Checkboxes: Landscape / Buildings
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
    [
        SNew(SHorizontalBox)

        + SHorizontalBox::Slot().AutoWidth().Padding(0,0,24,0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,6,0)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this]{ return bDoLandscape ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bDoLandscape = (S == ECheckBoxState::Checked); })
            ]
            + SHorizontalBox::Slot().VAlign(VAlign_Center)
            [ SNew(STextBlock).Text(LOCTEXT("DoLandscape","Landscape")) ]
        ]

        + SHorizontalBox::Slot().AutoWidth()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,6,0)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this]{ return bDoBuildings ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bDoBuildings = (S == ECheckBoxState::Checked); })
            ]
            + SHorizontalBox::Slot().VAlign(VAlign_Center)
            [ SNew(STextBlock).Text(LOCTEXT("DoBuildings","Buildings")) ]
        ]
    ]

    // Elevation source
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
    [
        SNew(SHorizontalBox)
        .Visibility_Lambda([this]{ return bDoLandscape ? EVisibility::Visible : EVisibility::Collapsed; })

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
        [
            SNew(SBox).WidthOverride(kLabelWidth)
            [ SNew(STextBlock).Text(LOCTEXT("ElevSource","Elevation source"))
              .ColorAndOpacity(kAccentGray) ]
        ]
        + SHorizontalBox::Slot()
        [
            SAssignNew(ElevSourceCombo, SComboBox<TSharedPtr<FString>>)
            .OptionsSource(&ElevSourceItems)
            .OnSelectionChanged(this, &SLevelToolPanel::OnElevSourceSelected)
            .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item){
                return SNew(STextBlock).Text(FText::FromString(*Item));
            })
            [ SNew(STextBlock).Text(this, &SLevelToolPanel::GetElevSourceText) ]
        ]
    ]

    // Building pool picker
    + SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
    [
        SNew(SHorizontalBox)
        .Visibility_Lambda([this]{ return bDoBuildings ? EVisibility::Visible : EVisibility::Collapsed; })

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
        [
            SNew(SBox).WidthOverride(kLabelWidth)
            [ SNew(STextBlock).Text(LOCTEXT("BuildingPool","Building pool"))
              .ColorAndOpacity(kAccentGray) ]
        ]
        + SHorizontalBox::Slot()
        [
            SNew(SObjectPropertyEntryBox)
            .AllowedClass(ULevelToolBuildingPool::StaticClass())
            .ObjectPath(this, &SLevelToolPanel::GetBuildingPoolPath)
            .OnObjectChanged(this, &SLevelToolPanel::OnBuildingPoolChanged)
            .AllowClear(true)
        ]
    ];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section: Actions + Progress
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SLevelToolPanel::BuildActionBar()
{
    return SNew(SHorizontalBox)
    .Clipping(EWidgetClipping::ClipToBounds)

    + SHorizontalBox::Slot().Padding(12.f, 8.f, 6.f, 8.f)
    [
        SNew(SButton)
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        .IsEnabled(this, &SLevelToolPanel::IsGenerateEnabled)
        .OnClicked(this, &SLevelToolPanel::OnGenerateClicked)
        .ButtonColorAndOpacity_Lambda([this]() -> FLinearColor {
            return IsGenerateEnabled() ? kAccentBlue * 0.85f : kAccentGray * 0.4f;
        })
        [
            SNew(STextBlock)
            .Text(LOCTEXT("GenerateBtn","▶  Generate Map"))
            .ColorAndOpacity(FLinearColor::White)
        ]
    ]

    + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 8.f, 12.f, 8.f)
    [
        SNew(SButton)
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        .IsEnabled(this, &SLevelToolPanel::IsCancelEnabled)
        .OnClicked(this, &SLevelToolPanel::OnCancelClicked)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("CancelBtn","✕ Cancel"))
            .ColorAndOpacity_Lambda([this]() -> FSlateColor {
                return IsCancelEnabled() ? FLinearColor::White : kAccentGray;
            })
        ]
    ];
}

TSharedRef<SWidget> SLevelToolPanel::BuildProgressBar()
{
    return SNew(SVerticalBox)

    + SVerticalBox::Slot().AutoHeight()
    [
        SNew(SProgressBar)
        .Percent(this, &SLevelToolPanel::GetProgressPercent)
        .FillColorAndOpacity(this, &SLevelToolPanel::GetProgressBarColor)
        .Style(FAppStyle::GetWidgetStyle<FProgressBarStyle>("ProgressBar"))
    ]

    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
    [
        SNew(STextBlock)
        .Text(this, &SLevelToolPanel::GetProgressStageText)
        .ColorAndOpacity(kAccentGray)
    ];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section: Log
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SLevelToolPanel::BuildLogSection()
{
    return SNew(SVerticalBox)

    + SVerticalBox::Slot().AutoHeight()
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().VAlign(VAlign_Center).Padding(kSectionPad)
        [ MakeSectionHeader(LOCTEXT("LogHeader","Log")) ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,12,0)
        [
            SNew(SButton)
            .ButtonStyle(FAppStyle::Get(), "FlatButton")
            .OnClicked(this, &SLevelToolPanel::OnClearLogClicked)
            [ SNew(STextBlock).Text(LOCTEXT("ClearLog","Clear"))
              .ColorAndOpacity(kAccentGray) ]
        ]
    ]

    + SVerticalBox::Slot()
    .FillHeight(1.f)
    .MaxHeight(240.f)
    .Padding(4.f, 0.f)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
        .Padding(4.f)
        [
            SAssignNew(LogListView, SListView<TSharedPtr<FString>>)
            .ListItemsSource(&LogEntries)
            .OnGenerateRow(this, &SLevelToolPanel::GenerateLogRow)
            .SelectionMode(ESelectionMode::None)
            .ScrollbarVisibility(EVisibility::Visible)
        ]
    ];
}

TSharedRef<SWidget> SLevelToolPanel::MakeSectionHeader(const FText& Label)
{
    return SNew(STextBlock)
    .Text(Label)
    .TextStyle(FAppStyle::Get(), "SmallText")
    .ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Preset helpers
// ─────────────────────────────────────────────────────────────────────────────

void SLevelToolPanel::RefreshPresetList()
{
    PresetItems.Empty();
    const ULevelToolSettings* S = ULevelToolSettings::Get();
    for (const FLevelToolCoordPreset& P : S->CoordPresets)
        PresetItems.Add(MakeShared<FString>(P.Name));

    // Built-in defaults if settings empty
    if (PresetItems.IsEmpty())
    {
        for (const FString& N : {
            FString(TEXT("Seoul_Jongno")),
            FString(TEXT("Chernobyl")),
            FString(TEXT("Pripyat")),
            FString(TEXT("Detroit_Downtown")),
            FString(TEXT("Incheon_Port"))})
        {
            PresetItems.Add(MakeShared<FString>(N));
        }
    }

    if (!PresetItems.IsEmpty())
        SelectedPreset = PresetItems[0];

    if (PresetCombo.IsValid())
        PresetCombo->RefreshOptions();
}

void SLevelToolPanel::OnPresetSelected(TSharedPtr<FString> Item, ESelectInfo::Type)
{
    SelectedPreset = Item;
}

FText SLevelToolPanel::GetSelectedPresetText() const
{
    return SelectedPreset.IsValid()
        ? FText::FromString(*SelectedPreset)
        : LOCTEXT("SelectPreset","Select preset...");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Coordinate inputs
// ─────────────────────────────────────────────────────────────────────────────

TOptional<float> SLevelToolPanel::GetLatValue()    const { return CurrentLat; }
TOptional<float> SLevelToolPanel::GetLonValue()    const { return CurrentLon; }
TOptional<float> SLevelToolPanel::GetRadiusValue() const { return CurrentRadius; }
void SLevelToolPanel::OnLatChanged(float V)    { CurrentLat    = V; }
void SLevelToolPanel::OnLonChanged(float V)    { CurrentLon    = V; }
void SLevelToolPanel::OnRadiusChanged(float V) { CurrentRadius = V; }
bool SLevelToolPanel::IsCustomCoordsEnabled()  const { return !bUsePreset; }

// ─────────────────────────────────────────────────────────────────────────────
//  Elevation source
// ─────────────────────────────────────────────────────────────────────────────

void SLevelToolPanel::OnElevSourceSelected(TSharedPtr<FString> Item, ESelectInfo::Type)
{
    SelectedElevSource = Item;

    // Mirror to settings
    ULevelToolSettings* S = ULevelToolSettings::Get();
    if (Item.IsValid())
    {
        if (Item->Contains(TEXT("OpenTopography")))
            S->ElevationSource = EElevationSource::OpenTopography;
        else if (Item->Contains(TEXT("Google")))
            S->ElevationSource = EElevationSource::GoogleMaps;
        else
            S->ElevationSource = EElevationSource::OpenElevation;
    }
}

FText SLevelToolPanel::GetElevSourceText() const
{
    return SelectedElevSource.IsValid()
        ? FText::FromString(*SelectedElevSource)
        : FText::GetEmpty();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Building pool
// ─────────────────────────────────────────────────────────────────────────────

FString SLevelToolPanel::GetBuildingPoolPath() const
{
    return BuildingPool.IsValid()
        ? BuildingPool->GetPathName()
        : FString();
}

void SLevelToolPanel::OnBuildingPoolChanged(const FAssetData& AssetData)
{
    BuildingPool = Cast<ULevelToolBuildingPool>(AssetData.GetAsset());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Action buttons
// ─────────────────────────────────────────────────────────────────────────────

FReply SLevelToolPanel::OnGenerateClicked()
{
    ULevelToolSubsystem* Sub = Subsystem.Get();
    if (!Sub) return FReply::Handled();

    // Validate settings first
    TArray<FString> Errors;
    if (!Sub->ValidateSettings(Errors))
    {
        FString Joined = FString::Join(Errors, TEXT("\n• "));
        // TODO: show modal dialog with errors
        OnLogLine(TEXT("⚠ Settings validation failed:"));
        for (const FString& E : Errors)
            OnLogLine(TEXT("  • ") + E);
        return FReply::Handled();
    }

    bJobFinished = false;
    CurrentProgress = 0.0f;
    CurrentStage    = TEXT("Starting...");

    FString Preset = (bUsePreset && SelectedPreset.IsValid()) ? *SelectedPreset : TEXT("");
    ULevelToolBuildingPool* Pool = bDoBuildings ? BuildingPool.Get() : nullptr;

    if (bDoLandscape && bDoBuildings)
        Sub->RunFullPipeline(Preset, CurrentLat, CurrentLon, CurrentRadius, Pool);
    else if (bDoLandscape)
        Sub->RunLandscapeOnly(Preset, CurrentLat, CurrentLon, CurrentRadius);
    else if (bDoBuildings)
        Sub->RunBuildingsOnly(Preset, CurrentLat, CurrentLon, CurrentRadius, Pool);

    return FReply::Handled();
}

FReply SLevelToolPanel::OnCancelClicked()
{
    if (ULevelToolSubsystem* Sub = Subsystem.Get())
        Sub->CancelJob();
    return FReply::Handled();
}

bool SLevelToolPanel::IsGenerateEnabled() const
{
    ULevelToolSubsystem* Sub = Subsystem.Get();
    return Sub && !Sub->IsRunning() && (bDoLandscape || bDoBuildings);
}

bool SLevelToolPanel::IsCancelEnabled() const
{
    ULevelToolSubsystem* Sub = Subsystem.Get();
    return Sub && Sub->IsRunning();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Progress bar
// ─────────────────────────────────────────────────────────────────────────────

TOptional<float> SLevelToolPanel::GetProgressPercent() const
{
    ULevelToolSubsystem* Sub = Subsystem.Get();
    if (Sub && Sub->IsRunning())
        return TOptional<float>(CurrentProgress);  // determinate
    return TOptional<float>();                       // indeterminate when idle
}

FText SLevelToolPanel::GetProgressStageText() const
{
    return FText::FromString(CurrentStage);
}

FSlateColor SLevelToolPanel::GetProgressBarColor() const
{
    if (bJobFinished)
        return bLastJobSucceeded ? kAccentGreen : kAccentRed;
    return kAccentBlue;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Log
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SLevelToolPanel::GenerateLogRow(
    TSharedPtr<FString> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    FLinearColor Color = FLinearColor(0.75f, 0.75f, 0.75f);
    if (Item->StartsWith(TEXT("✔"))) Color = kAccentGreen;
    else if (Item->StartsWith(TEXT("✖"))) Color = kAccentRed;
    else if (Item->StartsWith(TEXT("⚠"))) Color = FLinearColor(1.f, 0.8f, 0.2f);
    else if (Item->StartsWith(TEXT("▶"))) Color = kAccentBlue;

    return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
    .Padding(FMargin(4.f, 1.f))
    [
        SNew(STextBlock)
        .Text(FText::FromString(*Item))
        .ColorAndOpacity(Color)
        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
    ];
}

void SLevelToolPanel::ScrollLogToBottom()
{
    if (LogListView.IsValid() && !LogEntries.IsEmpty())
        LogListView->RequestScrollIntoView(LogEntries.Last());
}

FReply SLevelToolPanel::OnClearLogClicked()
{
    LogEntries.Empty();
    if (LogListView.IsValid())
        LogListView->RequestListRefresh();
    return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem event handlers
// ─────────────────────────────────────────────────────────────────────────────

void SLevelToolPanel::OnProgress(FString Stage, float Percent)
{
    CurrentProgress = Percent;
    CurrentStage    = Stage;
}

void SLevelToolPanel::OnComplete(bool bSuccess)
{
    bJobFinished       = true;
    bLastJobSucceeded  = bSuccess;
    CurrentProgress    = 1.0f;
    CurrentStage       = bSuccess ? TEXT("Done ✔") : TEXT("Failed ✖");
}

void SLevelToolPanel::OnLogLine(FString Line)
{
    LogEntries.Add(MakeShared<FString>(Line));
    if (LogListView.IsValid())
    {
        LogListView->RequestListRefresh();
        ScrollLogToBottom();
    }
}

#undef LOCTEXT_NAMESPACE
