#include "SBuildingEditorWindow.h"
#include "STilemapGrid.h"
#include "BuildingActor.h"
#include "BuildingDataAsset.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Selection.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "LevelEditorViewport.h"
#include "Engine/PointLight.h"
#include "Components/PointLightComponent.h"

#define LOCTEXT_NAMESPACE "BuildingEditor"

void SBuildingEditorWindow::Construct(const FArguments& InArgs)
{
    ChildSlot
    [
        SNew(SVerticalBox)

        // ── Toolbar ──
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            BuildToolbar()
        ]

        // ── Main: palette + grid ──
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(2.f)[ BuildFloorSelector() ]
                + SVerticalBox::Slot().AutoHeight().Padding(2.f)[ BuildTilePalette() ]
            ]

            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            .Padding(4.f)
            [
                SAssignNew(TilemapGrid, STilemapGrid)
                .FloorIndex(CurrentFloorIndex)
                .PaintTileType(CurrentPaintType)
                .OnTileClicked(FOnTileClicked::CreateSP(this, &SBuildingEditorWindow::OnTileClicked))
                .OnTileHovered(FOnTileHovered::CreateSP(this, &SBuildingEditorWindow::OnTileHovered))
            ]
        ]

        // ── Status bar ──
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(LOCTEXT("StatusReady", "Select a BuildingActor to begin editing"))
        ]
    ];

    RefreshTargetActor();
}

void SBuildingEditorWindow::Tick(
    const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    // Always try to refresh — pick up selection changes
    RefreshTargetActor();
}

void SBuildingEditorWindow::RefreshTargetActor()
{
    ABuildingActor* Actor = FindSelectedBuildingActor();
    if (Actor != TargetActor.Get())
    {
        TargetActor = Actor;
        if (TilemapGrid.IsValid())
        {
            TilemapGrid->SetBuildingActor(Actor);
        }
        if (Actor)
        {
            StatusText->SetText(FText::FromString(
                FString::Printf(TEXT("Editing: %s (%d floors)"),
                    *Actor->GetActorLabel(),
                    Actor->BuildingData.GetFloorCount())));
        }
        else
        {
            StatusText->SetText(LOCTEXT("StatusNoActor", "No BuildingActor selected"));
        }
    }
}

ABuildingActor* SBuildingEditorWindow::FindSelectedBuildingActor() const
{
    USelection* Selection = GEditor->GetSelectedActors();
    for (int32 i = 0; i < Selection->Num(); ++i)
    {
        if (ABuildingActor* Actor = Cast<ABuildingActor>(Selection->GetSelectedObject(i)))
        {
            return Actor;
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
//  Tile callbacks
// ─────────────────────────────────────────────────────────────────────

void SBuildingEditorWindow::OnTileClicked(int32 X, int32 Y, bool bCtrl)
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor) return;

    if (bCtrl)
    {
        StatusText->SetText(LOCTEXT("CtrlClickInfo",
            "Ctrl+Click: EditProxy mode (select tile in Details panel)"));
        return;
    }

    if (CurrentTool == EBuildingEditorTool::PlacePointLight)
    {
        SpawnPointLightAtTile(X, Y);
        return;
    }

    FScopedTransaction Transaction(LOCTEXT("PaintTile", "BuildingGenerator: Paint Tile"));
    Actor->Modify();
    Actor->SetTile(CurrentFloorIndex, X, Y, CurrentPaintType);

    FocusViewportOnTile(X, Y);

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Painted (%d, %d) -> %s"),
            X, Y, *UEnum::GetValueAsString(CurrentPaintType))));
}

void SBuildingEditorWindow::FocusViewportOnTile(int32 X, int32 Y)
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor)
        return;

    const FBuildingData& BD = Actor->BuildingData;
    const float S = BD.BuildingScale;

    FVector Pivot(BD.TileSize * 0.5f, BD.TileSize * 0.5f, BD.FloorHeight * 0.5f);
    FVector LocalPos = (BD.TileToWorld(CurrentFloorIndex, X, Y) + Pivot) * S;
    FVector WorldPos = Actor->GetActorTransform().TransformPosition(LocalPos);

    FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
    if (!VC)
        return;

    float TileExtent = BD.TileSize * S * 0.5f;
    FBox FocusBox(WorldPos - FVector(TileExtent), WorldPos + FVector(TileExtent));
    VC->FocusViewportOnBox(FocusBox, true);
}

void SBuildingEditorWindow::SpawnPointLightAtTile(int32 X, int32 Y)
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor) return;

    UWorld* World = Actor->GetWorld();
    if (!World) return;

    const FBuildingData& BD = Actor->BuildingData;
    const float S = BD.BuildingScale;

    FVector Pivot(BD.TileSize * 0.5f, BD.TileSize * 0.5f, BD.FloorHeight * 0.8f);
    FVector LocalPos = (BD.TileToWorld(CurrentFloorIndex, X, Y) + Pivot) * S;
    FVector WorldPos = Actor->GetActorTransform().TransformPosition(LocalPos);

    FScopedTransaction Transaction(LOCTEXT("PlaceLight", "BuildingGenerator: Place Point Light"));

    FActorSpawnParameters Params;
    Params.Owner = Actor;

    APointLight* Light = World->SpawnActor<APointLight>(
        APointLight::StaticClass(), WorldPos, FRotator::ZeroRotator, Params);

    if (Light)
    {
        FString FloorName = BD.Floors.IsValidIndex(CurrentFloorIndex)
            ? BD.Floors[CurrentFloorIndex].FloorName : TEXT("?");
        Light->SetActorLabel(FString::Printf(TEXT("BG_Light_%s_%d_%d"),
            *FloorName, X, Y));

        if (UPointLightComponent* LC = Light->PointLightComponent)
        {
            LC->SetAttenuationRadius(BD.TileSize * S * 2.5f);
            LC->SetIntensity(3000.f);
            LC->SetLightColor(FLinearColor(1.f, 0.95f, 0.85f));
            LC->SetCastShadows(true);
        }

        Light->AttachToActor(Actor, FAttachmentTransformRules::KeepWorldTransform);

        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(Light, true, true);
    }

    FocusViewportOnTile(X, Y);

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Placed PointLight at (%d, %d) on %s"),
            X, Y,
            BD.Floors.IsValidIndex(CurrentFloorIndex)
                ? *BD.Floors[CurrentFloorIndex].FloorName : TEXT("?"))));
}

void SBuildingEditorWindow::SetToolMode(EBuildingEditorTool NewTool)
{
    CurrentTool = NewTool;
    if (ToolModeLabel.IsValid())
    {
        FString ModeStr = (NewTool == EBuildingEditorTool::PlacePointLight)
            ? TEXT("[Point Light]")
            : TEXT("[Paint]");
        ToolModeLabel->SetText(FText::FromString(ModeStr));
    }
}

void SBuildingEditorWindow::OnTileHovered(int32 X, int32 Y)
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor || !Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex))
        return;

    ETileType Type = Actor->GetTileType(CurrentFloorIndex, X, Y);
    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Tile (%d, %d) = %s"),
            X, Y, *UEnum::GetValueAsString(Type))));
}

void SBuildingEditorWindow::OnFloorChanged(int32 NewFloor)
{
    CurrentFloorIndex = NewFloor;
    if (TilemapGrid.IsValid())
    {
        TilemapGrid->SetFloorIndex(NewFloor);
    }
    if (FloorLabel.IsValid())
    {
        ABuildingActor* Actor = TargetActor.Get();
        FString Name = (Actor && Actor->BuildingData.Floors.IsValidIndex(NewFloor))
            ? Actor->BuildingData.Floors[NewFloor].FloorName
            : FString::Printf(TEXT("Floor %d"), NewFloor);
        FloorLabel->SetText(FText::FromString(Name));
    }
}

FReply SBuildingEditorWindow::OnRebuildClicked()
{
    ABuildingActor* Actor = TargetActor.Get();
    if (Actor)
    {
        FScopedTransaction Transaction(LOCTEXT("Rebuild", "BuildingGenerator: Rebuild HISM"));
        Actor->Modify();
        Actor->RebuildHISM();
        StatusText->SetText(LOCTEXT("Rebuilt", "HISM rebuilt successfully"));
    }
    return FReply::Handled();
}

FReply SBuildingEditorWindow::OnAddFloorClicked()
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor) return FReply::Handled();

    FScopedTransaction Transaction(LOCTEXT("AddFloor", "BuildingGenerator: Add Floor (Copy)"));
    Actor->Modify();

    int32 Count = Actor->BuildingData.GetFloorCount();
    FString NewName = FString::Printf(TEXT("%dF"), Count + 1);

    if (Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex))
    {
        FFloorData SourceCopy = Actor->BuildingData.Floors[CurrentFloorIndex];
        FFloorData& NewFloor = Actor->BuildingData.Floors.AddDefaulted_GetRef();
        NewFloor = MoveTemp(SourceCopy);
        NewFloor.FloorName = NewName;
    }
    else
    {
        Actor->BuildingData.AddFloor(14, 10, NewName);
    }

    int32 NewIndex = Actor->BuildingData.GetFloorCount() - 1;
    OnFloorChanged(NewIndex);

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Added floor %s (copied from %s)"),
            *NewName,
            Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex)
                ? *Actor->BuildingData.Floors[CurrentFloorIndex].FloorName
                : TEXT("default"))));

    return FReply::Handled();
}

FReply SBuildingEditorWindow::OnCopyFloorClicked()
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor || !Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex))
    {
        StatusText->SetText(LOCTEXT("CopyFail", "No floor to copy"));
        return FReply::Handled();
    }

    ClipboardFloor = Actor->BuildingData.Floors[CurrentFloorIndex];

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Copied floor: %s (%dx%d)"),
            *ClipboardFloor->FloorName,
            ClipboardFloor->Width, ClipboardFloor->Height)));

    return FReply::Handled();
}

FReply SBuildingEditorWindow::OnPasteFloorClicked()
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor || !Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex))
    {
        StatusText->SetText(LOCTEXT("PasteNoFloor", "No target floor"));
        return FReply::Handled();
    }

    if (!ClipboardFloor.IsSet())
    {
        StatusText->SetText(LOCTEXT("PasteEmpty", "Clipboard empty — copy a floor first"));
        return FReply::Handled();
    }

    FScopedTransaction Transaction(LOCTEXT("PasteFloor", "BuildingGenerator: Paste Floor"));
    Actor->Modify();

    FFloorData& Target = Actor->BuildingData.Floors[CurrentFloorIndex];
    FString OriginalName = Target.FloorName;
    Target = ClipboardFloor.GetValue();
    Target.FloorName = OriginalName;

    Actor->RebuildHISM();

    if (TilemapGrid.IsValid())
    {
        TilemapGrid->Invalidate(EInvalidateWidgetReason::Paint);
    }

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Pasted to floor: %s"), *OriginalName)));

    return FReply::Handled();
}

FReply SBuildingEditorWindow::OnDeleteFloorClicked()
{
    ABuildingActor* Actor = TargetActor.Get();
    if (!Actor) return FReply::Handled();

    int32 Count = Actor->BuildingData.GetFloorCount();
    if (Count <= 1)
    {
        StatusText->SetText(LOCTEXT("DeleteLast", "Cannot delete the last floor"));
        return FReply::Handled();
    }

    if (!Actor->BuildingData.Floors.IsValidIndex(CurrentFloorIndex))
        return FReply::Handled();

    FScopedTransaction Transaction(LOCTEXT("DeleteFloor", "BuildingGenerator: Delete Floor"));
    Actor->Modify();

    FString Removed = Actor->BuildingData.Floors[CurrentFloorIndex].FloorName;
    Actor->BuildingData.Floors.RemoveAt(CurrentFloorIndex);

    int32 NewIndex = FMath::Min(CurrentFloorIndex, Actor->BuildingData.GetFloorCount() - 1);
    OnFloorChanged(NewIndex);

    Actor->RebuildHISM();

    StatusText->SetText(FText::FromString(
        FString::Printf(TEXT("Deleted floor: %s"), *Removed)));

    return FReply::Handled();
}

// ─────────────────────────────────────────────────────────────────────
//  UI builders
// ─────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SBuildingEditorWindow::BuildToolbar()
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("RebuildBtn", "Rebuild HISM"))
            .ToolTipText(LOCTEXT("RebuildTip", "Rebuild all HISM instances from tile data"))
            .OnClicked(FOnClicked::CreateSP(this, &SBuildingEditorWindow::OnRebuildClicked))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("AddFloorBtn", "Add Floor"))
            .ToolTipText(LOCTEXT("AddFloorTip", "Add new floor (copies current floor)"))
            .OnClicked(FOnClicked::CreateSP(this, &SBuildingEditorWindow::OnAddFloorClicked))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("CopyFloorBtn", "Copy"))
            .ToolTipText(LOCTEXT("CopyFloorTip", "Copy current floor to clipboard"))
            .OnClicked(FOnClicked::CreateSP(this, &SBuildingEditorWindow::OnCopyFloorClicked))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("PasteFloorBtn", "Paste"))
            .ToolTipText(LOCTEXT("PasteFloorTip", "Paste clipboard to current floor"))
            .OnClicked(FOnClicked::CreateSP(this, &SBuildingEditorWindow::OnPasteFloorClicked))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("DeleteFloorBtn", "Delete Floor"))
            .ToolTipText(LOCTEXT("DeleteFloorTip", "Delete current floor"))
            .OnClicked(FOnClicked::CreateSP(this, &SBuildingEditorWindow::OnDeleteFloorClicked))
        ];
}

TSharedRef<SWidget> SBuildingEditorWindow::BuildFloorSelector()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("FloorHeader", "Floor"))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("PrevFloor", "<"))
                .OnClicked_Lambda([this]()
                {
                    if (CurrentFloorIndex > 0) OnFloorChanged(CurrentFloorIndex - 1);
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Center)
            [
                SAssignNew(FloorLabel, STextBlock)
                .Text(FText::FromString(TEXT("1F")))
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("NextFloor", ">"))
                .OnClicked_Lambda([this]()
                {
                    ABuildingActor* A = TargetActor.Get();
                    int32 Max = A ? A->BuildingData.GetFloorCount() - 1 : 0;
                    if (CurrentFloorIndex < Max) OnFloorChanged(CurrentFloorIndex + 1);
                    return FReply::Handled();
                })
            ]
        ];
}

TSharedRef<SWidget> SBuildingEditorWindow::BuildTilePalette()
{
    struct FPaletteEntry { ETileType Type; FText Label; FLinearColor Color; };

    static const FPaletteEntry Entries[] = {
        { ETileType::Empty,       LOCTEXT("PE", "Empty"),       FLinearColor(0.96f, 0.96f, 0.96f) },
        { ETileType::Wall,        LOCTEXT("PW", "Wall"),        FLinearColor(0.29f, 0.29f, 0.29f) },
        { ETileType::Wall_Door,   LOCTEXT("PD", "Door"),        FLinearColor(1.00f, 0.84f, 0.00f) },
        { ETileType::Wall_Window, LOCTEXT("PWn", "Window"),     FLinearColor(0.53f, 0.81f, 0.92f) },
        { ETileType::Floor,       LOCTEXT("PF", "Floor"),       FLinearColor(0.88f, 0.88f, 0.88f) },
        { ETileType::Room_A,      LOCTEXT("PA", "Room A"),      FLinearColor(0.40f, 0.73f, 0.42f) },
        { ETileType::Room_B,      LOCTEXT("PB", "Room B"),      FLinearColor(0.67f, 0.28f, 0.74f) },
        { ETileType::Room_C,      LOCTEXT("PC", "Room C"),      FLinearColor(1.00f, 0.44f, 0.26f) },
        { ETileType::Corridor,    LOCTEXT("PCo", "Corridor"),   FLinearColor(0.74f, 0.74f, 0.74f) },
        { ETileType::Stairs_Up,   LOCTEXT("PSU", "Stairs Up"),  FLinearColor(1.00f, 0.55f, 0.00f) },
        { ETileType::Stairs_Down, LOCTEXT("PSD", "Stairs Down"),FLinearColor(0.90f, 0.45f, 0.00f) },
    };

    TSharedRef<SVerticalBox> VBox = SNew(SVerticalBox);

    VBox->AddSlot().AutoHeight()
    [
        SNew(STextBlock)
        .Text(LOCTEXT("PaletteHeader", "Tile Palette"))
        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
    ];

    for (const FPaletteEntry& E : Entries)
    {
        VBox->AddSlot().AutoHeight().Padding(0.f, 2.f)
        [
            SNew(SButton)
            .ButtonColorAndOpacity(E.Color)
            .OnClicked_Lambda([this, T = E.Type]()
            {
                CurrentPaintType = T;
                if (TilemapGrid.IsValid()) TilemapGrid->SetPaintTileType(T);
                SetToolMode(EBuildingEditorTool::Paint);
                return FReply::Handled();
            })
            [
                SNew(STextBlock)
                .Text(E.Label)
                .Justification(ETextJustify::Center)
                .ColorAndOpacity(E.Type == ETileType::Wall
                    ? FSlateColor(FLinearColor::White)
                    : FSlateColor(FLinearColor::Black))
            ]
        ];
    }

    VBox->AddSlot().AutoHeight().Padding(0.f, 8.f)
    [
        SNew(STextBlock)
        .Text(LOCTEXT("ActorTools", "Actor Tools"))
        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
    ];

    VBox->AddSlot().AutoHeight().Padding(0.f, 2.f)
    [
        SNew(SButton)
        .ButtonColorAndOpacity(FLinearColor(1.f, 0.92f, 0.5f))
        .OnClicked_Lambda([this]()
        {
            SetToolMode(EBuildingEditorTool::PlacePointLight);
            return FReply::Handled();
        })
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PointLightBtn", "Point Light"))
            .Justification(ETextJustify::Center)
            .ColorAndOpacity(FSlateColor(FLinearColor::Black))
        ]
    ];

    VBox->AddSlot().AutoHeight().Padding(0.f, 6.f)
    [
        SAssignNew(ToolModeLabel, STextBlock)
        .Text(LOCTEXT("ToolModePaint", "[Paint]"))
        .Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
        .ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
    ];

    return VBox;
}

#undef LOCTEXT_NAMESPACE
