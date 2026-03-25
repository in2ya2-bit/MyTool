#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "BuildingTypes.h"

class ABuildingActor;
class STilemapGrid;

enum class EBuildingEditorTool : uint8
{
    Paint,
    PlacePointLight,
    ToggleCeiling,
    ToggleFloor,
};

class SBuildingEditorWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SBuildingEditorWindow) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

private:
    TWeakObjectPtr<ABuildingActor> TargetActor;
    TSharedPtr<STilemapGrid> TilemapGrid;
    int32 CurrentFloorIndex = 0;
    ETileType CurrentPaintType = ETileType::Wall;
    EBuildingEditorTool CurrentTool = EBuildingEditorTool::Paint;

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> FloorLabel;
    TSharedPtr<STextBlock> ToolModeLabel;

    TOptional<FFloorData> ClipboardFloor;

    void OnTileClicked(int32 X, int32 Y, bool bCtrl);
    void OnTileHovered(int32 X, int32 Y);
    void FocusViewportOnTile(int32 X, int32 Y);
    void SpawnPointLightAtTile(int32 X, int32 Y);
    void OnFloorChanged(int32 NewFloor);
    void SetToolMode(EBuildingEditorTool NewTool);
    FReply OnRebuildClicked();
    FReply OnAddFloorClicked();
    FReply OnCopyFloorClicked();
    FReply OnPasteFloorClicked();
    FReply OnDeleteFloorClicked();

    TSharedRef<SWidget> BuildTilePalette();
    TSharedRef<SWidget> BuildFloorSelector();
    TSharedRef<SWidget> BuildToolbar();

    void RefreshTargetActor();
    ABuildingActor* FindSelectedBuildingActor() const;
};
