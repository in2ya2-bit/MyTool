#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "BuildingTypes.h"

class ABuildingActor;

DECLARE_DELEGATE_ThreeParams(FOnTileClicked, int32 /*X*/, int32 /*Y*/, bool /*bCtrl*/);
DECLARE_DELEGATE_TwoParams(FOnTileHovered, int32 /*X*/, int32 /*Y*/);

/**
 * SLeafWidget 기반 2D 타일맵 에디터 그리드.
 * Zoom (마우스휠), Pan (중간버튼 드래그), 가시영역 컬링.
 * ABuildingActor는 TWeakObjectPtr로 안전 참조.
 */
class STilemapGrid : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(STilemapGrid)
        : _FloorIndex(0)
        , _PaintTileType(ETileType::Wall)
    {}
        SLATE_ARGUMENT(int32, FloorIndex)
        SLATE_ARGUMENT(ETileType, PaintTileType)
        SLATE_EVENT(FOnTileClicked, OnTileClicked)
        SLATE_EVENT(FOnTileHovered, OnTileHovered)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void SetBuildingActor(ABuildingActor* InActor);
    void SetFloorIndex(int32 InIndex);
    void SetPaintTileType(ETileType InType);

    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
        int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

    virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

    virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

private:
    TWeakObjectPtr<ABuildingActor> BuildingActor;
    int32 FloorIndex = 0;
    ETileType PaintTileType = ETileType::Wall;

    float ZoomLevel = 1.f;
    FVector2D PanOffset = FVector2D::ZeroVector;
    bool bIsPanning = false;
    FVector2D LastMousePos = FVector2D::ZeroVector;
    mutable int32 HoveredTileX = -1;
    mutable int32 HoveredTileY = -1;

    FOnTileClicked OnTileClicked;
    FOnTileHovered OnTileHovered;

    static constexpr float BaseCellSize = 28.f;
    static constexpr float MinZoom = 0.25f;
    static constexpr float MaxZoom = 4.f;

    float GetCellSize() const { return BaseCellSize * ZoomLevel; }
    FIntPoint ScreenToTile(const FGeometry& Geom, const FVector2D& ScreenPos) const;
    FLinearColor GetTileColor(ETileType Type) const;
    FString GetTileLabel(const FTileData& Tile) const;
};
