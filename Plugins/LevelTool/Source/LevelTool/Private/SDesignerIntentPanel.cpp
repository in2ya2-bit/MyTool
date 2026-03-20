#include "SDesignerIntentPanel.h"
#include "DesignerIntentSubsystem.h"
#include "EditLayerManager.h"
#include "EditLayerApplicator.h"
#include "EditLayerTypes.h"
#include "PresetManager.h"
#include "PresetTypes.h"
#include "ChecklistEngine.h"
#include "ChecklistTypes.h"
#include "SuggestionTypes.h"
#include "HeatmapGenerator.h"
#include "HeatmapTypes.h"
#include "LevelToolSubsystem.h"

#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Views/SListView.h"
#include "Styling/AppStyle.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Components/LineBatchComponent.h"

#define LOCTEXT_NAMESPACE "DesignerIntent"

static const FLinearColor kAccent       (0.20f, 0.55f, 1.00f);
static const FLinearColor kActiveTab    (0.20f, 0.55f, 1.00f);
static const FLinearColor kInactiveTab  (0.35f, 0.35f, 0.35f);
static const FLinearColor kDimText      (0.55f, 0.55f, 0.55f);
static const FLinearColor kGreenAccent  (0.20f, 0.80f, 0.40f);
static const FLinearColor kRedAccent    (1.00f, 0.35f, 0.30f);
static const FMargin      kSecPad       (12.f, 10.f, 12.f, 6.f);
static const FMargin      kRowPad       (12.f, 3.f);

// ─────────────────────────────────────────────────────────────────────────────
//  Construct / Destruct
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::Construct(const FArguments& InArgs)
{
	IntentSubsystem = GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>();
	if (auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>())
	{
		LayerManager = LTS->GetEditLayerManager();
	}

	// 슬라이더 초기값 동기화
	if (auto* DIS = IntentSubsystem.Get())
	{
		for (const auto& KV : DIS->GetAllSliderStates())
		{
			SliderValues.Add(KV.Key, KV.Value.CurrentValue);
		}
		SliderInitHandle = DIS->OnSliderInitialized.AddSP(
			this, &SDesignerIntentPanel::OnSliderInitialized);
	}

	if (auto* LM = LayerManager.Get())
	{
		LayerChangeHandle = LM->OnLayersChanged.AddSP(
			this, &SDesignerIntentPanel::OnLayersChanged);
	}

	RefreshLayerList();

	ChildSlot
	[
		SNew(SVerticalBox)

		// ── 타이틀 ────────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 12.f, 12.f, 4.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Designer Intent"))
			.TextStyle(FAppStyle::Get(), "NormalText.Important")
			.ColorAndOpacity(FLinearColor::White)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(1.f) ]

		// ── 탭 바 ─────────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight()
		[ BuildTabBar() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		// ── 탭 컨텐츠 ────────────────────────────────────────────
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(TabSwitcher, SWidgetSwitcher)
			.WidgetIndex_Lambda([this]{ return ActiveTabIndex; })

			+ SWidgetSwitcher::Slot()[ BuildIntentTab() ]
			+ SWidgetSwitcher::Slot()[ BuildRulesetTab() ]
		]
	];
}

SDesignerIntentPanel::~SDesignerIntentPanel()
{
	if (auto* DIS = IntentSubsystem.Get())
	{
		DIS->OnSliderInitialized.Remove(SliderInitHandle);
	}
	if (auto* LM = LayerManager.Get())
	{
		LM->OnLayersChanged.Remove(LayerChangeHandle);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tab bar
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildTabBar()
{
	return SNew(SHorizontalBox)
	.Clipping(EWidgetClipping::ClipToBounds)

	+ SHorizontalBox::Slot().AutoWidth().Padding(12.f, 6.f, 4.f, 6.f)
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "FlatButton")
		.OnClicked_Lambda([this]{ return OnTabClicked(0); })
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TabIntent", "의도 수정"))
			.ColorAndOpacity_Lambda([this]{ return GetTabColor(0); })
		]
	]

	+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 6.f)
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "FlatButton")
		.OnClicked_Lambda([this]{ return OnTabClicked(1); })
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TabRuleset", "룰셋 옵션"))
			.ColorAndOpacity_Lambda([this]{ return GetTabColor(1); })
		]
	];
}

FReply SDesignerIntentPanel::OnTabClicked(int32 TabIndex)
{
	ActiveTabIndex = TabIndex;

	if (TabIndex == 1)
	{
		auto* DIS = IntentSubsystem.Get();
		if (DIS && DIS->IsStage1Ready() && DIS->GetChecklistEngine())
		{
			ERulesetType Ruleset = (SelectedRuleset == 0) ? ERulesetType::BR : ERulesetType::Extraction;
			DIS->GetChecklistEngine()->RunDiagnosis(Ruleset);
			RebuildChecklistResults();
			RebuildSuggestionCards();
		}
	}
	return FReply::Handled();
}

FSlateColor SDesignerIntentPanel::GetTabColor(int32 TabIndex) const
{
	return (ActiveTabIndex == TabIndex) ? FSlateColor(kActiveTab) : FSlateColor(kInactiveTab);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tab 0: 의도 수정
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildIntentTab()
{
	return SNew(SScrollBox)

	+ SScrollBox::Slot()
	[
		SNew(SVerticalBox)

		// ── 레퍼런스 프리셋 ─────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight()
		[ BuildPresetSection() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		// ── 슬라이더 ─────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight()
		[ BuildSliderSection() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		// ── Edit Layer 목록 ──────────────────────────────────────
		+ SVerticalBox::Slot().FillHeight(1.f)
		[ BuildLayerListSection() ]
	];
}

// ─────────────────────────────────────────────────────────────────────────────
//  프리셋 섹션
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildSelectionModeBar()
{
	auto MakeBtn = [this](EPresetSelectionMode Mode, const FText& Label)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Mode]
			{
				return (CurrentSelectionMode == Mode)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState S)
			{
				CurrentSelectionMode = (S == ECheckBoxState::Checked) ? Mode : EPresetSelectionMode::None;
			})
			[ SNew(STextBlock).Text(Label) ];
	};

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[ MakeBtn(EPresetSelectionMode::Building, LOCTEXT("SelBuilding", "건물")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[ MakeBtn(EPresetSelectionMode::Area, LOCTEXT("SelArea", "영역")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[ MakeBtn(EPresetSelectionMode::Road, LOCTEXT("SelRoad", "도로")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2)
		[ MakeBtn(EPresetSelectionMode::None, LOCTEXT("SelNone", "없음")) ];
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildPresetSection()
{
	auto* DIS = IntentSubsystem.Get();
	UPresetManager* PM = DIS ? DIS->GetPresetManager() : nullptr;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)

	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("PresetHeader", "레퍼런스 프리셋")) ]

	+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f)
	[ BuildSelectionModeBar() ];

	if (PM)
	{
		// BR 프리셋
		Box->AddSlot().AutoHeight().Padding(12.f, 4.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BRPresets", "Battle Royale"))
			.ColorAndOpacity(kAccent)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		];

		for (const FMapPreset& P : PM->GetReferencePresets())
		{
			if (P.Ruleset != ERulesetType::BR) continue;
			FString Name = P.PresetName;
			Box->AddSlot().AutoHeight().Padding(kRowPad)
			[
				SNew(SButton)
				.Text(FText::FromString(P.PresetName))
				.ToolTipText(FText::FromString(FString::Printf(
					TEXT("%s — %s\n밀도%.0f 개방%.0f 동선%.0f 고저차%.0f 파괴%.0f"),
					*P.ReferenceGame, *P.RecommendedCoords.Description,
					P.Sliders.UrbanDensity, P.Sliders.Openness,
					P.Sliders.RouteComplexity, P.Sliders.ElevationContrast,
					P.Sliders.DestructionLevel)))
				.OnClicked_Lambda([this, Name]{ return OnPresetClicked(Name); })
			];
		}

		// EX 프리셋
		Box->AddSlot().AutoHeight().Padding(12.f, 8.f, 12.f, 4.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("EXPresets", "Extraction"))
			.ColorAndOpacity(kGreenAccent)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		];

		for (const FMapPreset& P : PM->GetReferencePresets())
		{
			if (P.Ruleset != ERulesetType::Extraction) continue;
			FString Name = P.PresetName;
			Box->AddSlot().AutoHeight().Padding(kRowPad)
			[
				SNew(SButton)
				.Text(FText::FromString(P.PresetName))
				.ToolTipText(FText::FromString(FString::Printf(
					TEXT("%s — %s\n밀도%.0f 개방%.0f 동선%.0f 고저차%.0f 파괴%.0f"),
					*P.ReferenceGame, *P.RecommendedCoords.Description,
					P.Sliders.UrbanDensity, P.Sliders.Openness,
					P.Sliders.RouteComplexity, P.Sliders.ElevationContrast,
					P.Sliders.DestructionLevel)))
				.OnClicked_Lambda([this, Name]{ return OnPresetClicked(Name); })
			];
		}

		// 커스텀 프리셋 영역
		if (PM->GetCustomPresets().Num() > 0)
		{
			Box->AddSlot().AutoHeight().Padding(12.f, 8.f, 12.f, 4.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CustomPresets", "커스텀 프리셋"))
				.ColorAndOpacity(kDimText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			];

			for (const FMapPreset& P : PM->GetCustomPresets())
			{
				FString Name = P.PresetName;
				Box->AddSlot().AutoHeight().Padding(kRowPad)
				[
					SNew(SButton)
					.Text(FText::FromString(P.PresetName))
					.ToolTipText(FText::FromString(P.Description))
					.OnClicked_Lambda([this, Name]{ return OnPresetClicked(Name); })
				];
			}
		}

		// 현재 상태 저장 버튼
		Box->AddSlot().AutoHeight().Padding(12.f, 8.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SavePreset", "현재 상태를 프리셋으로 저장"))
			.OnClicked(this, &SDesignerIntentPanel::OnSaveCustomPreset)
		];
	}

	return Box;
}

FReply SDesignerIntentPanel::OnPresetClicked(FString PresetName)
{
	auto* DIS = IntentSubsystem.Get();
	UPresetManager* PM = DIS ? DIS->GetPresetManager() : nullptr;
	if (!PM) return FReply::Handled();

	const FMapPreset* Found = PM->FindPreset(PresetName);
	const bool bIsRef = Found && Found->bIsReference;

	PM->ApplyPreset(PresetName);

	if (DIS)
	{
		for (const auto& KV : DIS->GetAllSliderStates())
		{
			SliderValues.FindOrAdd(KV.Key) = KV.Value.CurrentValue;
		}
	}

	if (bIsRef && Found)
	{
		SelectedRuleset = (Found->Ruleset == ERulesetType::BR) ? 0 : 1;
		RebuildChecklistResults();
		RebuildSuggestionCards();
	}

	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnSaveCustomPreset()
{
	auto* DIS = IntentSubsystem.Get();
	UPresetManager* PM = DIS ? DIS->GetPresetManager() : nullptr;
	if (!PM) return FReply::Handled();

	const FString Name = FString::Printf(TEXT("Custom_%s"),
		*FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")));

	FMapPreset Preset = PM->MakePresetFromCurrentState(Name);
	PM->SaveCustomPreset(Preset);

	return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  슬라이더 섹션
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildSliderSection()
{
	return SNew(SVerticalBox)

	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("SliderHeader", "슬라이더")) ]

	+ SVerticalBox::Slot().AutoHeight()
	[ BuildSliderRow(ESliderType::UrbanDensity,     LOCTEXT("Density",  "도심 밀도")) ]
	+ SVerticalBox::Slot().AutoHeight()
	[ BuildSliderRow(ESliderType::Openness,         LOCTEXT("Open",     "개방도")) ]
	+ SVerticalBox::Slot().AutoHeight()
	[ BuildSliderRow(ESliderType::RouteComplexity,  LOCTEXT("Route",    "동선 복잡도")) ]
	+ SVerticalBox::Slot().AutoHeight()
	[ BuildSliderRow(ESliderType::ElevationContrast,LOCTEXT("Elev",     "고저차")) ]
	+ SVerticalBox::Slot().AutoHeight()
	[ BuildSliderRow(ESliderType::DestructionLevel, LOCTEXT("Destruct", "파괴도")) ]

	+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f, 12.f, 2.f)
	[
		SAssignNew(FitnessPreviewText, STextBlock)
		.Text(LOCTEXT("FitnessPreviewInit", ""))
		.ColorAndOpacity(kDimText)
	];
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildSliderRow(
	ESliderType Type, const FText& Label)
{
	SliderValues.FindOrAdd(Type, 0.f);

	return SNew(SHorizontalBox)
	.Clipping(EWidgetClipping::ClipToBounds)

	// 라벨
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(12.f, 4.f, 8.f, 4.f)
	[
		SNew(SBox).WidthOverride(80.f)
		[
			SNew(STextBlock)
			.Text(Label)
			.ColorAndOpacity(FLinearColor::White)
		]
	]

	// 슬라이더 바
	+ SHorizontalBox::Slot()
	.FillWidth(1.f)
	.VAlign(VAlign_Center)
	.Padding(0.f, 4.f)
	[
		SNew(SSlider)
		.Value_Lambda([this, Type]{ return GetSliderValue(Type) / 100.f; })
		.OnValueChanged_Lambda([this, Type](float V){ OnSliderChanged(V * 100.f, Type); })
		.OnMouseCaptureEnd_Lambda([this, Type]{ OnSliderCommitted(SliderValues.FindRef(Type), Type); })
		.SliderBarColor(kAccent)
	]

	// 현재값
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(8.f, 4.f, 4.f, 4.f)
	[
		SNew(SBox).WidthOverride(32.f)
		[
			SNew(STextBlock)
			.Text_Lambda([this, Type]{ return GetSliderValueText(Type); })
			.Justification(ETextJustify::Right)
			.ColorAndOpacity(FLinearColor::White)
		]
	]

	// 초기값 표시
	+ SHorizontalBox::Slot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.f, 4.f, 12.f, 4.f)
	[
		SNew(STextBlock)
		.Text_Lambda([this, Type]{ return GetSliderInitialText(Type); })
		.ColorAndOpacity(kDimText)
	];
}

float SDesignerIntentPanel::GetSliderValue(ESliderType Type) const
{
	const float* V = SliderValues.Find(Type);
	return V ? *V : 0.f;
}

void SDesignerIntentPanel::OnSliderChanged(float NewValue, ESliderType Type)
{
	SliderValues.FindOrAdd(Type) = NewValue;

	if (FitnessPreviewText.IsValid())
	{
		float PredictedScore = SimulateFitnessChange(Type, NewValue);

		auto* DIS = IntentSubsystem.Get();
		UChecklistEngine* Eng = DIS ? DIS->GetChecklistEngine() : nullptr;
		int32 TotalItems = 0, PassItems = 0;
		if (Eng)
		{
			for (const FCheckResult& CR : Eng->GetLastReport().Results)
			{
				if (CR.Status != ECheckStatus::NotApplicable) ++TotalItems;
				if (CR.Status == ECheckStatus::Pass) ++PassItems;
			}
		}
		float PassPct = (TotalItems > 0) ? (float)PassItems / TotalItems * 100.f : 0.f;

		float ErrMargin = DIS ? DIS->GetPredictionErrorMargin() : 5.f;
		FString SimText = FString::Printf(
			TEXT("예상 적합도: %.0f \u00B1%.0f  |  달성률: %.0f%% (%d/%d)"),
			PredictedScore, ErrMargin, PassPct, PassItems, TotalItems);

		if (Eng)
		{
			TArray<UChecklistEngine::FReferenceSimilarity> Sims = Eng->ComputeReferenceSimilarity();
			if (Sims.Num() > 0)
			{
				SimText += FString::Printf(TEXT("  |  유사: %s %.0f%%"),
					*Sims[0].PresetName, Sims[0].SimilarityPct);
			}
		}

		FitnessPreviewText->SetText(FText::FromString(SimText));
		FLinearColor C = (PredictedScore >= 70.f) ? kGreenAccent :
		                 (PredictedScore >= 40.f) ? FLinearColor(1.f, 0.85f, 0.2f) : kRedAccent;
		FitnessPreviewText->SetColorAndOpacity(C);
	}
}

void SDesignerIntentPanel::OnSliderCommitted(float NewValue, ESliderType Type)
{
	float PredictedBefore = SimulateFitnessChange(Type, NewValue);

	if (auto* DIS = IntentSubsystem.Get())
	{
		DIS->ApplySlider(Type, NewValue);

		UChecklistEngine* Eng = DIS->GetChecklistEngine();
		if (Eng)
		{
			ERulesetType RS = (SelectedRuleset == 0) ? ERulesetType::BR : ERulesetType::Extraction;
			Eng->RunDiagnosis(RS);
			float ActualScore = Eng->GetLastReport().TotalScore;
			DIS->RecordPredictionSample(PredictedBefore, ActualScore);
		}
	}
}

FText SDesignerIntentPanel::GetSliderValueText(ESliderType Type) const
{
	return FText::AsNumber(FMath::RoundToInt(GetSliderValue(Type)));
}

FText SDesignerIntentPanel::GetSliderInitialText(ESliderType Type) const
{
	auto* DIS = IntentSubsystem.Get();
	if (!DIS) return FText::GetEmpty();

	const float Init = DIS->GetSliderInitialValue(Type);
	return FText::Format(
		LOCTEXT("InitFmt", "\u25C4 초기값: {0}"),
		FText::AsNumber(FMath::RoundToInt(Init)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Edit Layer 목록
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildLayerFilterBar()
{
	const TArray<TPair<FString, FText>> Filters = {
		{ TEXT("Slider"),    LOCTEXT("FilterSlider",  "슬라이더") },
		{ TEXT("Preset"),    LOCTEXT("FilterPreset",  "프리셋")  },
		{ TEXT("AiSuggest"), LOCTEXT("FilterAI",      "AI 제안") },
		{ TEXT("Manual"),    LOCTEXT("FilterManual",  "수동")    },
	};

	TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
	for (const auto& F : Filters)
	{
		FString Key = F.Key;
		Bar->AddSlot().AutoWidth().Padding(2)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, Key]{ return ActiveFilters.Contains(Key) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Key](ECheckBoxState S){
				if (S == ECheckBoxState::Checked) ActiveFilters.Add(Key); else ActiveFilters.Remove(Key);
				RefreshLayerList();
			})
			.Content()
			[
				SNew(STextBlock).Text(F.Value).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			]
		];
	}
	return Bar;
}

FReply SDesignerIntentPanel::OnToggleFilter(FString FilterKey)
{
	if (ActiveFilters.Contains(FilterKey))
		ActiveFilters.Remove(FilterKey);
	else
		ActiveFilters.Add(FilterKey);
	RefreshLayerList();
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnHideGroup(FString GroupKey)
{
	auto* LM = LayerManager.Get();
	if (!LM) return FReply::Handled();

	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (GetGroupKey(L) == GroupKey)
		{
			LM->SetLayerVisible(L.LayerId, false);
		}
	}
	RefreshLayerList();
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnDeleteGroup(FString GroupKey)
{
	auto* LM = LayerManager.Get();
	if (!LM) return FReply::Handled();

	TArray<FString> IdsToDelete;
	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (GetGroupKey(L) == GroupKey)
			IdsToDelete.Add(L.LayerId);
	}
	for (const FString& Id : IdsToDelete)
	{
		LM->RemoveLayer(Id);
	}
	RefreshLayerList();
	return FReply::Handled();
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildLayerListSection()
{
	return SNew(SVerticalBox)

	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[ MakeSectionHeader(LOCTEXT("LayerHeader", "Edit Layer 목록")) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,12,0)
		[
			SNew(STextBlock)
			.Text_Lambda([this]{
				auto* LM = LayerManager.Get();
				if (!LM) return FText::GetEmpty();
				int32 BAdd=0, BRem=0, RAdd=0, RBlk=0, Trn=0, Mrk=0, Other=0;
				for (const FEditLayer& L : LM->GetAllLayers())
				{
					switch(L.Type)
					{
					case EEditLayerType::BuildingAdd: case EEditLayerType::BuildingAddBatch: ++BAdd; break;
					case EEditLayerType::BuildingRemove: ++BRem; break;
					case EEditLayerType::RoadAdd: ++RAdd; break;
					case EEditLayerType::RoadBlock: ++RBlk; break;
					case EEditLayerType::TerrainModify: ++Trn; break;
					case EEditLayerType::Marker: ++Mrk; break;
					default: ++Other; break;
					}
				}
				return FText::FromString(FString::Printf(
					TEXT("총 %d | 건물+%d -%d | 도로+%d -%d | 지형%d | 마커%d"),
					LM->GetLayerCount(), BAdd, BRem, RAdd, RBlk, Trn, Mrk));
			})
			.ColorAndOpacity(kDimText)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
		]
	]

	+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f, 12.f, 4.f)
	[ BuildLayerFilterBar() ]

	+ SVerticalBox::Slot().FillHeight(1.f).Padding(12.f, 0.f, 12.f, 8.f)
	[
		SAssignNew(LayerListView, SListView<TSharedPtr<FString>>)
		.ListItemsSource(&LayerIds)
		.OnGenerateRow(this, &SDesignerIntentPanel::GenerateLayerRow)
		.SelectionMode(ESelectionMode::None)
	];
}

TSharedRef<ITableRow> SDesignerIntentPanel::GenerateLayerRow(
	TSharedPtr<FString> LayerId,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	FString Id = *LayerId;

	if (Id.StartsWith(TEXT("GROUP::")))
	{
		FString GroupLabel = Id.RightChop(7);
		bool bFolded = FoldedGroups.Contains(Id);
		FString Arrow = bFolded ? TEXT("\u25B6 ") : TEXT("\u25BC ");

		return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton")
				.OnClicked_Lambda([this, Id]{ return OnToggleGroupFold(Id); })
				[
					SNew(STextBlock)
					.Text(FText::FromString(Arrow + GroupLabel))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FLinearColor(0.7f, 0.85f, 1.0f))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton")
				.ToolTipText(LOCTEXT("HideGroupTip", "그룹 전체 숨기기"))
				.OnClicked_Lambda([this, Id]{ return OnHideGroup(Id); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("\U0001F6AB"))) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton")
				.ToolTipText(LOCTEXT("DeleteGroupTip", "그룹 전체 삭제"))
				.OnClicked_Lambda([this, Id]{ return OnDeleteGroup(Id); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("\u2716"))) ]
			]
		];
	}

	auto* LM = LayerManager.Get();
	const FEditLayer* Layer = LM ? LM->FindLayer(Id) : nullptr;

	FText LabelText = Layer
		? FText::FromString(Layer->Label)
		: FText::FromString(Id);

	bool bVisible = Layer ? Layer->bVisible : true;
	bool bLocked  = Layer ? Layer->bLocked : false;
	bool bConflict = ConflictWarnings.Contains(Id);

	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 2, 4, 2)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "FlatButton")
			.OnClicked_Lambda([this, Id]{ return OnToggleLayerVisible(Id); })
			[
				SNew(STextBlock)
				.Text(bVisible ? FText::FromString(TEXT("\U0001F441")) : FText::FromString(TEXT("\U0001F6AB")))
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 2, 4, 2)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "FlatButton")
			.ToolTipText(LOCTEXT("LockToggleTip", "레이어 잠금/해제"))
			.OnClicked_Lambda([this, Id]{
				auto* LM2 = LayerManager.Get();
				if (LM2)
				{
					FEditLayer* L = LM2->FindLayer(Id);
					if (L) LM2->SetLayerLocked(Id, !L->bLocked);
				}
				RefreshLayerList();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(bLocked ? FText::FromString(TEXT("\U0001F512")) : FText::FromString(TEXT("\U0001F513")))
			]
		]

		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 2)
		[
			SNew(STextBlock)
			.Text(LabelText)
			.ColorAndOpacity(bVisible ? (bConflict ? FLinearColor(1.f, 0.7f, 0.2f) : FLinearColor::White) : kDimText)
			.ToolTipText(bConflict
				? FText::FromString(ConflictWarnings.FindRef(Id))
				: FText::GetEmpty())
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 2, 0, 2)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "FlatButton")
			.IsEnabled(!bLocked)
			.OnClicked_Lambda([this, Id]{ return OnDeleteLayer(Id); })
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("\u2715")))
				.ColorAndOpacity(kRedAccent)
			]
		]
	];
}

FString SDesignerIntentPanel::GetGroupKey(const FEditLayer& Layer) const
{
	if (!Layer.SourceId.IsEmpty())
	{
		switch (Layer.CreatedBy)
		{
		case ELayerCreatedBy::Slider:
			return FString::Printf(TEXT("GROUP::Slider_%s"), *Layer.SourceId);
		case ELayerCreatedBy::Preset:
			return FString::Printf(TEXT("GROUP::Preset_%s"), *Layer.SourceId);
		case ELayerCreatedBy::ReferencePreset:
			return FString::Printf(TEXT("GROUP::Preset_%s"), *Layer.SourceId);
		default:
			break;
		}
	}

	switch (Layer.CreatedBy)
	{
	case ELayerCreatedBy::Slider:    return TEXT("GROUP::Slider");
	case ELayerCreatedBy::Preset:    return TEXT("GROUP::Preset");
	case ELayerCreatedBy::ReferencePreset: return TEXT("GROUP::Preset");
	case ELayerCreatedBy::AiSuggest: return TEXT("GROUP::AiSuggest");
	case ELayerCreatedBy::Manual:    return TEXT("GROUP::Manual");
	default:                         return TEXT("GROUP::기타");
	}
}

FReply SDesignerIntentPanel::OnToggleGroupFold(FString GroupKey)
{
	if (FoldedGroups.Contains(GroupKey))
		FoldedGroups.Remove(GroupKey);
	else
		FoldedGroups.Add(GroupKey);
	RefreshLayerList();
	return FReply::Handled();
}

void SDesignerIntentPanel::RefreshLayerList()
{
	LayerIds.Empty();
	auto* LM = LayerManager.Get();
	if (!LM)
	{
		if (LayerListView.IsValid()) LayerListView->RequestListRefresh();
		return;
	}

	TMap<FString, TArray<FString>> Groups;
	TArray<FString> GroupOrder;

	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (ActiveFilters.Num() > 0)
		{
			FString GK = GetGroupKey(L);
			FString FilterKey = GK.RightChop(7);
			if (!ActiveFilters.Contains(FilterKey)) continue;
		}

		FString GK = GetGroupKey(L);
		if (!Groups.Contains(GK))
		{
			Groups.Add(GK, {});
			GroupOrder.Add(GK);
		}
		Groups[GK].Add(L.LayerId);
	}

	for (const FString& GK : GroupOrder)
	{
		LayerIds.Add(MakeShared<FString>(GK));
		if (!FoldedGroups.Contains(GK))
		{
			for (const FString& Id : Groups[GK])
			{
				LayerIds.Add(MakeShared<FString>(Id));
			}
		}
	}

	ConflictWarnings.Empty();
	if (LM)
	{
		TArray<UEditLayerManager::FLayerConflict> Conflicts = LM->DetectLayerConflicts();
		for (const auto& C : Conflicts)
		{
			ConflictWarnings.Add(C.LayerIdA, C.Reason);
			ConflictWarnings.Add(C.LayerIdB, C.Reason);
		}
	}

	if (LayerListView.IsValid())
	{
		LayerListView->RequestListRefresh();
	}
}

FReply SDesignerIntentPanel::OnToggleLayerVisible(FString LayerId)
{
	auto* LM = LayerManager.Get();
	if (!LM || !LM->GetApplicator()) return FReply::Handled();

	const FEditLayer* Layer = LM->FindLayer(LayerId);
	if (!Layer) return FReply::Handled();

	UEditLayerApplicator* Applicator = LM->GetApplicator();
	if (Layer->bVisible)
	{
		Applicator->HideLayer(LayerId);
	}
	else
	{
		Applicator->ApplyLayer(LayerId);
	}

	LM->SaveToJson();
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnDeleteLayer(FString LayerId)
{
	auto* LM = LayerManager.Get();
	if (!LM || !LM->GetApplicator()) return FReply::Handled();

	LM->GetApplicator()->DeleteLayer(LayerId);
	LM->SaveToJson();
	return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tab 1: 룰셋 옵션 (S5+ 구현 예정 — 프레임만 구성)
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildRulesetTab()
{
	return SNew(SScrollBox)
	+ SScrollBox::Slot()
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[ BuildRulesetSelector() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		+ SVerticalBox::Slot().AutoHeight()
		[ BuildDashboardSection() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		+ SVerticalBox::Slot().AutoHeight()
		[ BuildChecklistSection() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		+ SVerticalBox::Slot().AutoHeight()
		[ BuildSuggestionSection() ]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(SSeparator).Thickness(0.5f).ColorAndOpacity(kInactiveTab * 0.5f) ]

		+ SVerticalBox::Slot().AutoHeight()
		[ BuildHeatmapToggleSection() ]
	];
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildRulesetSelector()
{
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("RulesetHeader", "룰셋 선택")) ]

	+ SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this]{ return SelectedRuleset == 0
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S)
				{ if (S == ECheckBoxState::Checked) SelectedRuleset = 0; })
			[
				SNew(STextBlock).Text(LOCTEXT("BR", "Battle Royale"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this]{ return SelectedRuleset == 1
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState S)
				{ if (S == ECheckBoxState::Checked) SelectedRuleset = 1; })
			[
				SNew(STextBlock).Text(LOCTEXT("EX", "Extraction"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.OnClicked(this, &SDesignerIntentPanel::OnRunDiagnosis)
			[
				SNew(STextBlock).Text(LOCTEXT("RunCheck", "체크 실행"))
			]
		]
	];
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildChecklistSection()
{
	return SNew(SVerticalBox)

	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[ MakeSectionHeader(LOCTEXT("CheckHeader", "체크리스트 결과")) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
		[
			SAssignNew(ScoreText, STextBlock)
			.Text(LOCTEXT("ScoreNone", ""))
			.ColorAndOpacity(kDimText)
		]
	]

	+ SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
	[
		SAssignNew(CheckResultBox, SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("CheckPlaceholder", "체크 실행 후 결과가 여기에 표시됩니다."))
			.ColorAndOpacity(kDimText)
		]
	];
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildSuggestionSection()
{
	return SNew(SVerticalBox)

	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("SuggestHeader", "제안 카드")) ]

	+ SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
	[
		SAssignNew(SuggestionBox, SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SuggestPlaceholder", "Fail 항목에 대한 제안이 여기에 표시됩니다."))
			.ColorAndOpacity(kDimText)
		]
	]

	+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 4.f, 12.f, 8.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("AcceptAll", "모두 수락"))
			.OnClicked_Lambda([this]{
				auto* DIS = IntentSubsystem.Get();
				UChecklistEngine* Eng = DIS ? DIS->GetChecklistEngine() : nullptr;
				if (Eng) { Eng->AcceptAllSuggestions(); RebuildSuggestionCards(); }
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("RejectAll", "모두 거절"))
			.OnClicked_Lambda([this]{
				auto* DIS = IntentSubsystem.Get();
				UChecklistEngine* Eng = DIS ? DIS->GetChecklistEngine() : nullptr;
				if (Eng) { Eng->RejectAllSuggestions(); RebuildSuggestionCards(); }
				return FReply::Handled();
			})
		]
	];
}

FReply SDesignerIntentPanel::OnRunDiagnosis()
{
	auto* DIS = IntentSubsystem.Get();
	if (!DIS || !DIS->GetChecklistEngine()) return FReply::Handled();

	ERulesetType Ruleset = (SelectedRuleset == 0) ? ERulesetType::BR : ERulesetType::Extraction;
	DIS->GetChecklistEngine()->RunDiagnosis(Ruleset);
	RebuildChecklistResults();
	return FReply::Handled();
}

void SDesignerIntentPanel::RebuildChecklistResults()
{
	auto* DIS = IntentSubsystem.Get();
	if (!DIS || !DIS->GetChecklistEngine()) return;

	const FCheckReport& Report = DIS->GetChecklistEngine()->GetLastReport();

	// 총점 표시
	if (ScoreText.IsValid())
	{
		FLinearColor ScoreColor = (Report.TotalScore >= 70.f) ? kGreenAccent :
		                          (Report.TotalScore >= 40.f) ? FLinearColor(1.f, 0.85f, 0.2f) : kRedAccent;
		ScoreText->SetText(FText::Format(
			LOCTEXT("ScoreFmt", "총점: {0}/100  (P:{1} W:{2} F:{3})"),
			FText::AsNumber(FMath::RoundToInt(Report.TotalScore)),
			FText::AsNumber(Report.CountByStatus(ECheckStatus::Pass)),
			FText::AsNumber(Report.CountByStatus(ECheckStatus::Warning)),
			FText::AsNumber(Report.CountByStatus(ECheckStatus::Fail))));
		ScoreText->SetColorAndOpacity(ScoreColor);
	}

	// 체크 결과 목록 재구축
	if (CheckResultBox.IsValid())
	{
		CheckResultBox->ClearChildren();
		for (const FCheckResult& R : Report.Results)
		{
			CheckResultBox->AddSlot().AutoHeight()
			[ BuildCheckResultRow(R) ];
		}
	}

	RebuildSuggestionCards();
	RebuildDashboard();
}

TSharedRef<SWidget> SDesignerIntentPanel::BuildCheckResultRow(const FCheckResult& R)
{
	FVector Loc = R.FocusLocation;
	bool bHasLocation = !Loc.IsNearlyZero(1.f);

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
	.Clipping(EWidgetClipping::ClipToBounds)

	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 2, 6, 2)
	[
		SNew(STextBlock)
		.Text(StatusIcon((uint8)R.Status))
		.ColorAndOpacity(StatusColor((uint8)R.Status))
	]

	+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 2, 8, 2)
	[
		SNew(SBox).WidthOverride(48.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(R.CheckId))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(FLinearColor::White)
		]
	]

	+ SHorizontalBox::Slot().FillWidth(0.4f).VAlign(VAlign_Center).Padding(0, 2)
	[
		SNew(STextBlock)
		.Text(FText::FromString(R.Label))
		.ColorAndOpacity(FLinearColor::White)
	]

	+ SHorizontalBox::Slot().FillWidth(0.6f).VAlign(VAlign_Center).Padding(4, 2, 12, 2)
	[
		SNew(STextBlock)
		.Text(FText::FromString(R.Detail))
		.ColorAndOpacity(kDimText)
		.AutoWrapText(true)
	];

	if (bHasLocation)
	{
		TArray<FString> Actors = R.HighlightActors;
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "FlatButton")
			.OnClicked_Lambda([Loc]{ FocusViewportOn(Loc); return FReply::Handled(); })
			.OnHovered_Lambda([this, Actors]{ HighlightActors(Actors); })
			.OnUnhovered_Lambda([this]{ ClearHighlight(); })
			.ToolTipText(LOCTEXT("ClickToFocus", "클릭하면 뷰포트에서 해당 위치로 이동합니다"))
			[ Row ];
	}

	return Row;
}

FLinearColor SDesignerIntentPanel::StatusColor(uint8 Status) const
{
	switch ((ECheckStatus)Status)
	{
	case ECheckStatus::Pass:          return kGreenAccent;
	case ECheckStatus::Warning:       return FLinearColor(1.f, 0.85f, 0.2f);
	case ECheckStatus::Fail:          return kRedAccent;
	case ECheckStatus::NotApplicable: return kDimText;
	default:                           return kDimText;
	}
}

FText SDesignerIntentPanel::StatusIcon(uint8 Status) const
{
	switch ((ECheckStatus)Status)
	{
	case ECheckStatus::Pass:          return FText::FromString(TEXT("\u2705"));
	case ECheckStatus::Warning:       return FText::FromString(TEXT("\u26A0"));
	case ECheckStatus::Fail:          return FText::FromString(TEXT("\u274C"));
	case ECheckStatus::NotApplicable: return FText::FromString(TEXT("\u2B1C"));
	default:                           return FText::FromString(TEXT("?"));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  제안 카드 수락/거절 + 재구축
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::RebuildSuggestionCards()
{
	if (!SuggestionBox.IsValid()) return;
	SuggestionBox->ClearChildren();

	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (!Engine) return;

	const TArray<FSuggestionCard>& Cards = Engine->GetSuggestions();
	if (Cards.Num() == 0)
	{
		SuggestionBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoSuggestCards", "제안 없음 — 모든 항목 Pass"))
			.ColorAndOpacity(kGreenAccent)
		];
		return;
	}

	for (const FSuggestionCard& Card : Cards)
	{
		FString CapturedId = Card.CardId;
		bool bPending = (Card.Status == ESuggestionStatus::Pending);

		SuggestionBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.f, 6.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("[%s] %s"),
						*Card.CheckId, *Card.Problem)))
					.ColorAndOpacity(bPending ? kRedAccent : kDimText)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Card.Reference))
					.ColorAndOpacity(kDimText)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("제안: %s"),
						*Card.SuggestedLayer.Label)))
					.ColorAndOpacity(FLinearColor::White)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					bPending
					? StaticCastSharedRef<SWidget>(
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("FocusCard", "\xF0\x9F\x93\x8D 위치 보기"))
							.ToolTipText(LOCTEXT("FocusCardTip", "뷰포트를 제안 위치로 이동합니다"))
							.OnClicked_Lambda([this, CapturedId]{ return OnFocusSuggestion(CapturedId); })
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("Accept", "수락"))
							.OnClicked_Lambda([this, CapturedId]{ return OnAcceptSuggestion(CapturedId); })
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("Reject", "거절"))
							.OnClicked_Lambda([this, CapturedId]{ return OnRejectSuggestion(CapturedId); })
						]
					)
					: StaticCastSharedRef<SWidget>(
						SNew(STextBlock)
						.Text(FText::FromString(Card.GetStatusText()))
						.ColorAndOpacity(Card.Status == ESuggestionStatus::Accepted ? kGreenAccent : kDimText)
					)
				]
			]
		];
	}
}

FReply SDesignerIntentPanel::OnFocusSuggestion(FString CardId)
{
	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (!Engine) return FReply::Handled();

	for (const FSuggestionCard& Card : Engine->GetSuggestions())
	{
		if (Card.CardId != CardId) continue;
		if (!Card.FocusLocation.IsNearlyZero(1.f))
		{
			FocusViewportOn(Card.FocusLocation, 20000.f);
			ShowFocusRing(Card.FocusLocation);
		}
		break;
	}
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnAcceptSuggestion(FString CardId)
{
	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (Engine)
	{
		for (const FSuggestionCard& Card : Engine->GetSuggestions())
		{
			if (Card.CardId == CardId && !Card.FocusLocation.IsNearlyZero(1.f))
			{
				FocusViewportOn(Card.FocusLocation, 20000.f);
				ShowFocusRing(Card.FocusLocation);
				break;
			}
		}

		Engine->AcceptSuggestion(CardId);
		RebuildSuggestionCards();
		RefreshLayerList();
	}
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnRejectSuggestion(FString CardId)
{
	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (Engine)
	{
		Engine->RejectSuggestion(CardId);
		RebuildSuggestionCards();
	}
	ClearFocusRing();
	return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  대시보드 (적합도 + 유사도)
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildDashboardSection()
{
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("DashHeader", "맵 적합도 대시보드")) ]
	+ SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
	[
		SAssignNew(DashboardBox, SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DashPlaceholder", "체크 실행 후 대시보드가 표시됩니다."))
			.ColorAndOpacity(kDimText)
		]
	];
}

void SDesignerIntentPanel::RebuildDashboard()
{
	if (!DashboardBox.IsValid()) return;
	DashboardBox->ClearChildren();

	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	if (!Engine) return;

	const FCheckReport& Report = Engine->GetLastReport();

	// 적합도 바
	FLinearColor FitnessColor = (Report.TotalScore >= 70.f) ? kGreenAccent :
	                             (Report.TotalScore >= 40.f) ? FLinearColor(1.f, 0.85f, 0.2f) : kRedAccent;

	FString RulesetName = (Report.Ruleset == ERulesetType::BR) ? TEXT("BR") : TEXT("EX");

	DashboardBox->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(TEXT("%s 설계 기준 적합도: %.0f / 100"),
			*RulesetName, Report.TotalScore)))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		.ColorAndOpacity(FitnessColor)
	];

	DashboardBox->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SProgressBar)
		.Percent(Report.TotalScore / 100.f)
		.FillColorAndOpacity(FitnessColor)
	];

	// 최대 감점 항목 (상위 3개)
	TArray<const FCheckResult*> Sorted;
	for (const FCheckResult& R : Report.Results)
	{
		if (R.Status == ECheckStatus::Fail || R.Status == ECheckStatus::Warning)
			Sorted.Add(&R);
	}
	Sorted.Sort([](const FCheckResult& A, const FCheckResult& B)
	{
		return A.Score < B.Score;
	});

	if (Sorted.Num() > 0)
	{
		DashboardBox->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TopDeductions", "주요 감점:"))
			.ColorAndOpacity(kDimText)
		];

		for (int32 i = 0; i < FMath::Min(3, Sorted.Num()); ++i)
		{
			const FCheckResult* CR = Sorted[i];
			float W = (Report.Ruleset == ERulesetType::BR)
				? UChecklistEngine::GetBRWeight(CR->CheckId)
				: UChecklistEngine::GetEXWeight(CR->CheckId);
			float Deduction = W * ((CR->Status == ECheckStatus::Fail) ? 1.f : 0.5f);

			DashboardBox->AddSlot().AutoHeight().Padding(8, 1)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s %s %s (-%.*f)"),
					*StatusIcon((uint8)CR->Status).ToString(),
					*CR->CheckId, *CR->Label, 1, Deduction)))
				.ColorAndOpacity(StatusColor((uint8)CR->Status))
			];
		}
	}

	// 레퍼런스 유사도
	TArray<UChecklistEngine::FReferenceSimilarity> Similarities = Engine->ComputeReferenceSimilarity();

	if (Similarities.Num() > 0)
	{
		DashboardBox->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RefSimilarity", "가장 유사한 레퍼런스:"))
			.ColorAndOpacity(kDimText)
		];

		for (int32 i = 0; i < FMath::Min(3, Similarities.Num()); ++i)
		{
			const auto& Sim = Similarities[i];
			DashboardBox->AddSlot().AutoHeight().Padding(8, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%d위 %s"),
						i + 1, *Sim.PresetName)))
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SProgressBar)
					.Percent(Sim.SimilarityPct / 100.f)
					.FillColorAndOpacity(kAccent)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%.0f%%"), Sim.SimilarityPct)))
					.ColorAndOpacity(FLinearColor::White)
				]
			];
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  히트맵 토글
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::BuildHeatmapToggleSection()
{
	return SNew(SVerticalBox)
	+ SVerticalBox::Slot().AutoHeight().Padding(kSecPad)
	[ MakeSectionHeader(LOCTEXT("HeatmapHeader", "히트맵 오버레이")) ]

	+ SVerticalBox::Slot().AutoHeight().Padding(kRowPad)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("HM_Density", "건물 밀도"))
			.ToolTipText(LOCTEXT("HM_DensityTip", "건물 밀도 히트맵 (빨강=과밀, 초록=적정)"))
			.OnClicked_Lambda([this]{ return OnToggleHeatmap(EHeatmapMode::BuildingDensity); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("HM_Roads", "도로 연결성"))
			.OnClicked_Lambda([this]{ return OnToggleHeatmap(EHeatmapMode::RoadConnectivity); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("HM_Pois", "거점 분포"))
			.OnClicked_Lambda([this]{ return OnToggleHeatmap(EHeatmapMode::PoiDistribution); })
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("HM_Elev", "고저차"))
			.OnClicked_Lambda([this]{ return OnToggleHeatmap(EHeatmapMode::Elevation); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("HM_Off", "끄기"))
			.OnClicked_Lambda([this]{ return OnHideHeatmap(); })
		]
	];
}

// ─────────────────────────────────────────────────────────────────────────────
//  공통 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SDesignerIntentPanel::MakeSectionHeader(const FText& Label)
{
	return SNew(STextBlock)
		.Text(Label)
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  이벤트 핸들러
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::OnSliderInitialized(const FString& /*MapId*/)
{
	if (auto* DIS = IntentSubsystem.Get())
	{
		for (const auto& KV : DIS->GetAllSliderStates())
		{
			SliderValues.FindOrAdd(KV.Key) = KV.Value.CurrentValue;
		}
	}

	// LayerManager 갱신 (1단계 완료 후 새로 생성되었을 수 있음)
	if (auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>())
	{
		auto* NewLM = LTS->GetEditLayerManager();
		if (NewLM != LayerManager.Get())
		{
			if (auto* OldLM = LayerManager.Get())
			{
				OldLM->OnLayersChanged.Remove(LayerChangeHandle);
			}
			LayerManager = NewLM;
			if (NewLM)
			{
				LayerChangeHandle = NewLM->OnLayersChanged.AddSP(
					this, &SDesignerIntentPanel::OnLayersChanged);
			}
		}
	}

	RefreshLayerList();
}

void SDesignerIntentPanel::OnLayersChanged()
{
	RefreshLayerList();
}

// ─────────────────────────────────────────────────────────────────────────────
//  히트맵 오버레이 토글
// ─────────────────────────────────────────────────────────────────────────────

FReply SDesignerIntentPanel::OnToggleHeatmap(EHeatmapMode Mode)
{
	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	UHeatmapGenerator* HeatGen = Engine ? Engine->GetHeatmapGenerator() : nullptr;
	if (!HeatGen) return FReply::Handled();

	if (HeatGen->IsOverlayVisible() && HeatGen->GetActiveOverlayMode() == Mode)
	{
		HeatGen->HideHeatmapOverlay();
	}
	else
	{
		if (!HeatGen->HasCachedData()) HeatGen->GenerateAll();
		HeatGen->ShowHeatmapOverlay(Mode);
	}
	return FReply::Handled();
}

FReply SDesignerIntentPanel::OnHideHeatmap()
{
	auto* DIS = IntentSubsystem.Get();
	UChecklistEngine* Engine = DIS ? DIS->GetChecklistEngine() : nullptr;
	UHeatmapGenerator* HeatGen = Engine ? Engine->GetHeatmapGenerator() : nullptr;
	if (HeatGen) HeatGen->HideHeatmapOverlay();
	return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  P2-14: 적합도 시뮬레이션 — 슬라이더 드래그 중 예상 점수
// ─────────────────────────────────────────────────────────────────────────────

float SDesignerIntentPanel::SimulateFitnessChange(ESliderType Type, float NewValue) const
{
	auto* DIS = IntentSubsystem.Get();
	if (!DIS) return 0.f;

	UChecklistEngine* Engine = DIS->GetChecklistEngine();
	if (!Engine) return 0.f;

	const FCheckReport& Last = Engine->GetLastReport();
	float BaseScore = Last.TotalScore;

	float OldVal = DIS->GetSliderState(Type).CurrentValue;
	float Delta = NewValue - OldVal;

	float DeltaScore = 0.f;
	switch (Type)
	{
	case ESliderType::UrbanDensity:
		DeltaScore = Delta * 0.15f;
		break;
	case ESliderType::Openness:
		DeltaScore = Delta * 0.10f;
		break;
	case ESliderType::RouteComplexity:
		DeltaScore = Delta * 0.12f;
		break;
	case ESliderType::ElevationContrast:
		DeltaScore = Delta * 0.08f;
		break;
	case ESliderType::DestructionLevel:
		DeltaScore = Delta * -0.05f;
		break;
	}

	float Corrected = DeltaScore * DIS->GetCorrectionScale() + DIS->GetCorrectionBias();
	return FMath::Clamp(BaseScore + Corrected, 0.f, 100.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  P2-13: 호버 하이라이트
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::HighlightActors(const TArray<FString>& StableIds)
{
	ClearHighlight();
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		for (const FString& SId : StableIds)
		{
			if (A->Tags.Contains(*SId) || A->GetActorLabel().Contains(SId))
			{
				A->SetIsTemporarilyHiddenInEditor(false);
				GEditor->SelectActor(A, true, true);
				HighlightedActors.Add(A);
				break;
			}
		}
	}
}

void SDesignerIntentPanel::ClearHighlight()
{
	if (GEditor)
	{
		GEditor->SelectNone(true, true);
	}
	HighlightedActors.Empty();
}

void SDesignerIntentPanel::SetActorsCustomDepth(const TArray<AActor*>& Actors, bool bEnable, int32 StencilValue)
{
	for (AActor* A : Actors)
	{
		if (!A) continue;
		TArray<UPrimitiveComponent*> Prims;
		A->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			P->SetRenderCustomDepth(bEnable);
			if (bEnable) P->SetCustomDepthStencilValue(StencilValue);
		}
	}

	if (bEnable)
	{
		for (AActor* A : Actors)
			PreviewHighlightedActors.Add(A);
	}
	else
	{
		PreviewHighlightedActors.Empty();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  P2-12: 뷰포트 카메라 포커스
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::FocusViewportOn(const FVector& Location, float Distance)
{
	if (!GEditor) return;
	for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
	{
		if (!VC || !VC->IsPerspective()) continue;
		VC->SetViewLocation(Location + FVector(0, 0, Distance * 0.5f) - VC->GetViewRotation().Vector() * Distance);
		VC->SetLookAtLocation(Location);
		VC->Invalidate();
		break;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  제안 카드 포커스 링 (뷰포트 시각 표시)
// ─────────────────────────────────────────────────────────────────────────────

void SDesignerIntentPanel::ShowFocusRing(const FVector& Location)
{
	ClearFocusRing();

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const float RingRadius = 5000.f;
	const int32 Segments   = 48;
	const float RingHeight = 500.f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const FTransform SpawnTransform(FRotator::ZeroRotator, Location);
	AActor* RingActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnTransform, Params);
	if (!RingActor) return;

	RingActor->SetActorLabel(TEXT("LevelTool_FocusRing"));
	RingActor->Tags.Add(FName(TEXT("LevelTool_FocusRing")));
	RingActor->SetFolderPath(TEXT("LevelTool/Temp"));

	ULineBatchComponent* LineBatch = NewObject<ULineBatchComponent>(RingActor);
	LineBatch->SetupAttachment(RingActor->GetRootComponent());
	LineBatch->RegisterComponent();

	const FColor RingColor(0, 200, 255, 255);
	const float Thickness = 8.f;

	for (int32 Ring = 0; Ring < 3; ++Ring)
	{
		float R = RingRadius + Ring * 800.f;
		float Z = Location.Z + RingHeight + Ring * 200.f;

		for (int32 i = 0; i < Segments; ++i)
		{
			float A0 = (float)i / Segments * 2.f * PI;
			float A1 = (float)(i + 1) / Segments * 2.f * PI;
			FVector P0(Location.X + FMath::Cos(A0) * R, Location.Y + FMath::Sin(A0) * R, Z);
			FVector P1(Location.X + FMath::Cos(A1) * R, Location.Y + FMath::Sin(A1) * R, Z);
			LineBatch->DrawLine(P0, P1, RingColor, SDPG_World, Thickness);
		}
	}

	// Vertical pillar lines from ground to ring
	for (int32 i = 0; i < 4; ++i)
	{
		float Angle = (float)i / 4.f * 2.f * PI;
		FVector Base(Location.X + FMath::Cos(Angle) * RingRadius * 0.3f,
		             Location.Y + FMath::Sin(Angle) * RingRadius * 0.3f,
		             Location.Z);
		FVector Top = Base + FVector(0, 0, RingHeight + 800.f);
		LineBatch->DrawLine(Base, Top, FColor(0, 200, 255, 180), SDPG_World, 4.f);
	}

	// Center crosshair
	const float Cross = 2000.f;
	float CZ = Location.Z + 200.f;
	LineBatch->DrawLine(
		FVector(Location.X - Cross, Location.Y, CZ),
		FVector(Location.X + Cross, Location.Y, CZ),
		FColor(255, 100, 0, 255), SDPG_World, 6.f);
	LineBatch->DrawLine(
		FVector(Location.X, Location.Y - Cross, CZ),
		FVector(Location.X, Location.Y + Cross, CZ),
		FColor(255, 100, 0, 255), SDPG_World, 6.f);

	FocusRingActor = RingActor;

	// Auto-clear after 8 seconds
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle, [WeakThis = TWeakPtr<SDesignerIntentPanel>(StaticCastSharedRef<SDesignerIntentPanel>(AsShared()))]()
	{
		if (auto Pin = WeakThis.Pin())
			Pin->ClearFocusRing();
	}, 8.f, false);
}

void SDesignerIntentPanel::ClearFocusRing()
{
	if (AActor* A = FocusRingActor.Get())
	{
		A->Destroy();
	}
	FocusRingActor = nullptr;
}

#undef LOCTEXT_NAMESPACE
