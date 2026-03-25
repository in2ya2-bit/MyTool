#include "STilemapGrid.h"
#include "BuildingActor.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"

void STilemapGrid::Construct(const FArguments& InArgs)
{
    FloorIndex = InArgs._FloorIndex;
    PaintTileType = InArgs._PaintTileType;
    OnTileClicked = InArgs._OnTileClicked;
    OnTileHovered = InArgs._OnTileHovered;
}

void STilemapGrid::SetBuildingActor(ABuildingActor* InActor)
{
    BuildingActor = InActor;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void STilemapGrid::SetFloorIndex(int32 InIndex)
{
    FloorIndex = InIndex;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void STilemapGrid::SetPaintTileType(ETileType InType)
{
    PaintTileType = InType;
}

FVector2D STilemapGrid::ComputeDesiredSize(float) const
{
    return FVector2D(600.0, 400.0);
}

FIntPoint STilemapGrid::ScreenToTile(
    const FGeometry& Geom, const FVector2D& ScreenPos) const
{
    FVector2D LocalPos = Geom.AbsoluteToLocal(ScreenPos);
    float CellSize = GetCellSize();
    int32 X = FMath::FloorToInt32((LocalPos.X - PanOffset.X) / CellSize);
    int32 Y = FMath::FloorToInt32((LocalPos.Y - PanOffset.Y) / CellSize);
    return FIntPoint(X, Y);
}

FLinearColor STilemapGrid::GetTileColor(ETileType Type) const
{
    switch (Type)
    {
    case ETileType::Empty:       return FLinearColor(0.22f, 0.22f, 0.24f);
    case ETileType::Floor:       return FLinearColor(0.82f, 0.75f, 0.62f);
    case ETileType::Wall:        return FLinearColor(0.45f, 0.45f, 0.48f);
    case ETileType::Wall_Door:   return FLinearColor(1.00f, 0.84f, 0.00f);
    case ETileType::Wall_Window: return FLinearColor(0.53f, 0.81f, 0.92f);
    case ETileType::Stairs:
    case ETileType::Stairs_Up:   return FLinearColor(1.00f, 0.55f, 0.00f);
    case ETileType::Stairs_Down: return FLinearColor(0.90f, 0.45f, 0.00f);
    case ETileType::Room_A:      return FLinearColor(0.40f, 0.73f, 0.42f);
    case ETileType::Room_B:      return FLinearColor(0.67f, 0.28f, 0.74f);
    case ETileType::Room_C:      return FLinearColor(1.00f, 0.44f, 0.26f);
    case ETileType::Corridor:    return FLinearColor(0.60f, 0.68f, 0.72f);
    default:                     return FLinearColor::White;
    }
}

FString STilemapGrid::GetTileLabel(const FTileData& Tile) const
{
    if (Tile.TileType == ETileType::Wall)
    {
        const TCHAR* Base = TEXT("W");
        switch (Tile.WallVariant)
        {
        case EWallVariant::Isolated:   Base = TEXT("\u25A1"); break;
        case EWallVariant::Straight:   Base = TEXT("\u2500"); break;
        case EWallVariant::Corner:     Base = TEXT("\u2510"); break;
        case EWallVariant::T_Junction: Base = TEXT("\u2524"); break;
        case EWallVariant::Cross:      Base = TEXT("\u253C"); break;
        case EWallVariant::End:        Base = TEXT("\u2574"); break;
        default: break;
        }

        if (Tile.ManualYawOffset != 0.f)
        {
            int32 Dir = FMath::RoundToInt32(FMath::Fmod(Tile.ManualYawOffset + 360.f, 360.f)) / 90;
            const TCHAR* Rot = TEXT("");
            switch (Dir % 4)
            {
            case 1: Rot = TEXT("\u21BB"); break;
            case 2: Rot = TEXT("\u21C5"); break;
            case 3: Rot = TEXT("\u21BA"); break;
            default: break;
            }
            return FString::Printf(TEXT("%s%s"), Base, Rot);
        }

        return Base;
    }

    if (Tile.IsStairs())
    {
        int32 Dir = FMath::RoundToInt32(FMath::Fmod(Tile.AutoRotationYaw + 360.f, 360.f)) / 90;
        bool bUp = (Tile.TileType == ETileType::Stairs_Up || Tile.TileType == ETileType::Stairs);
        switch (Dir % 4)
        {
        case 0: return bUp ? TEXT("\u25B2") : TEXT("\u25BC");  // ▲ ▼
        case 1: return bUp ? TEXT("\u25B6") : TEXT("\u25C0");  // ▶ ◀
        case 2: return bUp ? TEXT("\u25BC") : TEXT("\u25B2");  // ▼ ▲
        case 3: return bUp ? TEXT("\u25C0") : TEXT("\u25B6");  // ◀ ▶
        default: return TEXT("S");
        }
    }

    auto YawArrow = [](float Yaw) -> const TCHAR*
    {
        int32 Dir = FMath::RoundToInt32(FMath::Fmod(Yaw + 360.f, 360.f)) / 90;
        switch (Dir % 4)
        {
        case 0: return TEXT("");
        case 1: return TEXT("\u21BB");  // ↻ 90°
        case 2: return TEXT("\u21C5");  // ⇅ 180°
        case 3: return TEXT("\u21BA");  // ↺ 270°
        default: return TEXT("");
        }
    };

    if (Tile.ManualYawOffset != 0.f)
    {
        const TCHAR* Arrow = YawArrow(Tile.ManualYawOffset);
        switch (Tile.TileType)
        {
        case ETileType::Wall:        return FString::Printf(TEXT("\u2593%s"), Arrow);
        case ETileType::Wall_Door:   return FString::Printf(TEXT("D%s"), Arrow);
        case ETileType::Wall_Window: return FString::Printf(TEXT("W%s"), Arrow);
        case ETileType::Floor:       return FString::Printf(TEXT("%s"), Arrow);
        case ETileType::Room_A:      return FString::Printf(TEXT("A%s"), Arrow);
        case ETileType::Room_B:      return FString::Printf(TEXT("B%s"), Arrow);
        case ETileType::Room_C:      return FString::Printf(TEXT("C%s"), Arrow);
        case ETileType::Corridor:    return FString::Printf(TEXT("\u00B7%s"), Arrow);
        default:                     return FString(Arrow);
        }
    }

    switch (Tile.TileType)
    {
    case ETileType::Wall_Door:   return TEXT("D");
    case ETileType::Wall_Window: return TEXT("W");
    case ETileType::Room_A:      return TEXT("A");
    case ETileType::Room_B:      return TEXT("B");
    case ETileType::Room_C:      return TEXT("C");
    case ETileType::Corridor:    return TEXT("\u00B7");
    default:                     return TEXT("");
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Rendering
// ─────────────────────────────────────────────────────────────────────

int32 STilemapGrid::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    const FSlateBrush* Brush = FCoreStyle::Get().GetBrush("GenericWhiteBox");
    if (!Brush)
    {
        Brush = FAppStyle::GetBrush("WhiteBrush");
    }

    ABuildingActor* Actor = BuildingActor.Get();
    if (!Actor || !Actor->BuildingData.Floors.IsValidIndex(FloorIndex))
    {
        // No floor data — draw placeholder
        FSlateDrawElement::MakeBox(
            OutDrawElements, LayerId,
            AllottedGeometry.ToPaintGeometry(),
            Brush, ESlateDrawEffect::None,
            FLinearColor(0.12f, 0.12f, 0.12f));

        const FSlateFontInfo HintFont = FCoreStyle::GetDefaultFontStyle("Bold", 12);
        FSlateDrawElement::MakeText(
            OutDrawElements, LayerId + 1,
            AllottedGeometry.ToPaintGeometry(),
            FString(TEXT("No floor data — click Add Floor")),
            HintFont, ESlateDrawEffect::None,
            FLinearColor(0.6f, 0.6f, 0.6f));

        return LayerId + 2;
    }

    const FFloorData& Floor = Actor->BuildingData.Floors[FloorIndex];
    const float CellSize = GetCellSize();
    const float Gap = 1.f;
    const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(
        "Regular", FMath::Max(6, FMath::RoundToInt32(8 * ZoomLevel)));

    // Background
    FSlateDrawElement::MakeBox(
        OutDrawElements, LayerId,
        AllottedGeometry.ToPaintGeometry(),
        Brush, ESlateDrawEffect::None,
        FLinearColor(0.12f, 0.12f, 0.12f));

    // Visible area culling
    FVector2D WidgetSize = AllottedGeometry.GetLocalSize();
    int32 MinVisX = FMath::Max(0, FMath::FloorToInt32(-PanOffset.X / CellSize));
    int32 MinVisY = FMath::Max(0, FMath::FloorToInt32(-PanOffset.Y / CellSize));
    int32 MaxVisX = FMath::Min(Floor.Width - 1,
        FMath::FloorToInt32((WidgetSize.X - PanOffset.X) / CellSize));
    int32 MaxVisY = FMath::Min(Floor.Height - 1,
        FMath::FloorToInt32((WidgetSize.Y - PanOffset.Y) / CellSize));

    for (int32 Y = MinVisY; Y <= MaxVisY; ++Y)
    {
        for (int32 X = MinVisX; X <= MaxVisX; ++X)
        {
            const FTileData& Tile = Floor.GetTile(X, Y);
            float CellX = PanOffset.X + X * CellSize;
            float CellY = PanOffset.Y + Y * CellSize;

            FLinearColor CellColor = GetTileColor(Tile.TileType);
            if (X == HoveredTileX && Y == HoveredTileY)
            {
                CellColor = FLinearColor::LerpUsingHSV(CellColor, FLinearColor::White, 0.3f);
            }

            // Cell fill
            FGeometry CellGeo = AllottedGeometry.MakeChild(
                FVector2D(CellSize - Gap, CellSize - Gap),
                FSlateLayoutTransform(FVector2D(CellX + Gap * 0.5, CellY + Gap * 0.5)));

            FSlateDrawElement::MakeBox(
                OutDrawElements, LayerId + 1,
                CellGeo.ToPaintGeometry(),
                Brush, ESlateDrawEffect::None,
                CellColor);

            // Layer indicator overlay (top-right corner)
            if (ZoomLevel >= 0.5f && (Tile.bNoCeiling || Tile.bNoFloor))
            {
                const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle(
                    "Bold", FMath::Max(5, FMath::RoundToInt32(6 * ZoomLevel)));
                TSharedRef<FSlateFontMeasure> SmFM =
                    FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

                FString Indicator;
                if (Tile.bNoCeiling && Tile.bNoFloor)
                    Indicator = TEXT("\u2300");   // ⌀ both removed
                else if (Tile.bNoCeiling)
                    Indicator = TEXT("\u2191X");  // ↑X ceiling off
                else
                    Indicator = TEXT("\u2193X");  // ↓X floor off

                FVector2D IndSize = SmFM->Measure(Indicator, SmallFont);
                FGeometry IndGeo = AllottedGeometry.MakeChild(
                    IndSize,
                    FSlateLayoutTransform(FVector2D(
                        CellX + CellSize - IndSize.X - 2.f,
                        CellY + 1.f)));

                FSlateDrawElement::MakeText(
                    OutDrawElements, LayerId + 3,
                    IndGeo.ToPaintGeometry(),
                    Indicator, SmallFont,
                    ESlateDrawEffect::None,
                    FLinearColor(1.f, 0.3f, 0.3f));
            }

            // Text label
            FString Label = GetTileLabel(Tile);
            if (!Label.IsEmpty() && ZoomLevel >= 0.5f)
            {
                TSharedRef<FSlateFontMeasure> FontMeasure =
                    FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
                FVector2D TextSize = FontMeasure->Measure(Label, Font);

                FGeometry TextGeo = AllottedGeometry.MakeChild(
                    TextSize,
                    FSlateLayoutTransform(FVector2D(
                        CellX + (CellSize - TextSize.X) * 0.5,
                        CellY + (CellSize - TextSize.Y) * 0.5)));

                FLinearColor TextColor =
                    (Tile.TileType == ETileType::Wall || Tile.TileType == ETileType::Empty)
                    ? FLinearColor::White
                    : FLinearColor(0.15f, 0.15f, 0.15f);

                FSlateDrawElement::MakeText(
                    OutDrawElements, LayerId + 2,
                    TextGeo.ToPaintGeometry(),
                    Label, Font,
                    ESlateDrawEffect::None,
                    TextColor);
            }
        }
    }

    return LayerId + 4;
}

// ─────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────

FReply STilemapGrid::OnMouseButtonDown(
    const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton)
    {
        bIsPanning = true;
        LastMousePos = MouseEvent.GetScreenSpacePosition();
        return FReply::Handled().CaptureMouse(SharedThis(this));
    }

    if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
    {
        FIntPoint TilePos = ScreenToTile(MyGeometry, MouseEvent.GetScreenSpacePosition());
        ABuildingActor* Actor = BuildingActor.Get();
        if (Actor && Actor->BuildingData.Floors.IsValidIndex(FloorIndex))
        {
            const FFloorData& Floor = Actor->BuildingData.Floors[FloorIndex];
            if (Floor.IsValidCoord(TilePos.X, TilePos.Y))
            {
                OnTileClicked.ExecuteIfBound(TilePos.X, TilePos.Y, MouseEvent.IsControlDown());
            }
        }
        return FReply::Handled();
    }

    return FReply::Unhandled();
}

FReply STilemapGrid::OnMouseButtonUp(
    const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton && bIsPanning)
    {
        bIsPanning = false;
        return FReply::Handled().ReleaseMouseCapture();
    }
    return FReply::Unhandled();
}

FReply STilemapGrid::OnMouseMove(
    const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (bIsPanning)
    {
        FVector2D Delta = MouseEvent.GetScreenSpacePosition() - LastMousePos;
        PanOffset += Delta;
        LastMousePos = MouseEvent.GetScreenSpacePosition();
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled();
    }

    FIntPoint TilePos = ScreenToTile(MyGeometry, MouseEvent.GetScreenSpacePosition());
    if (TilePos.X != HoveredTileX || TilePos.Y != HoveredTileY)
    {
        HoveredTileX = TilePos.X;
        HoveredTileY = TilePos.Y;
        OnTileHovered.ExecuteIfBound(TilePos.X, TilePos.Y);
        Invalidate(EInvalidateWidgetReason::Paint);
    }

    return FReply::Unhandled();
}

FReply STilemapGrid::OnMouseWheel(
    const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    float Delta = MouseEvent.GetWheelDelta();
    float OldZoom = ZoomLevel;
    ZoomLevel = FMath::Clamp(ZoomLevel + Delta * 0.1f, MinZoom, MaxZoom);

    if (!FMath::IsNearlyEqual(ZoomLevel, OldZoom))
    {
        FVector2D LocalMouse = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
        float Ratio = ZoomLevel / OldZoom;
        PanOffset = LocalMouse - (LocalMouse - PanOffset) * Ratio;
        Invalidate(EInvalidateWidgetReason::Paint);
    }

    return FReply::Handled();
}

FCursorReply STilemapGrid::OnCursorQuery(
    const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
    if (bIsPanning)
        return FCursorReply::Cursor(EMouseCursor::GrabHand);
    return FCursorReply::Cursor(EMouseCursor::Crosshairs);
}
