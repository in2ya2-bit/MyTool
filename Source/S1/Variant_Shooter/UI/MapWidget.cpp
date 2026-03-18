// Copyright Epic Games, Inc. All Rights Reserved.

#include "MapWidget.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/SizeBox.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/TextureRenderTarget2D.h"

void UMapWidget::NativeConstruct()
{
	Super::NativeConstruct();

	ApplyRenderTargetToImage();
	ApplyMapSize();
}

// ---------------------------------------------------------------------------
// Controller interface
// ---------------------------------------------------------------------------

void UMapWidget::SetRenderTarget(UTextureRenderTarget2D* InTarget)
{
	MapRenderTarget = InTarget;
	ApplyRenderTargetToImage();
	BP_OnRenderTargetSet(InTarget);
}

void UMapWidget::SetMapMode(bool bFull)
{
	bIsFullMap = bFull;
	ApplyMapSize();
	BP_OnMapModeChanged(bFull);
}

void UMapWidget::UpdateMarkers(const TArray<FMapMarkerData>& NewMarkers)
{
	if (MarkerPanel)
	{
		EnsureMarkerPoolSize(NewMarkers.Num());

		const float DisplaySize = bIsFullMap ? FullMapDisplaySize : MinimapDisplaySize;

		for (int32 i = 0; i < NewMarkers.Num(); ++i)
		{
			const FMapMarkerData& Data = NewMarkers[i];
			FMarkerWidgetEntry& Entry = MarkerPool[i];

			// --- Icon ---
			Entry.Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			Entry.Icon->SetColorAndOpacity(Data.Color);

			float Size = PlayerMarkerSize;
			if (Data.Type == EMapMarkerType::Teammate)
			{
				Size = TeammateMarkerSize;
			}
			else if (Data.Type == EMapMarkerType::Objective)
			{
				Size = ObjectiveMarkerSize;
			}

			if (UCanvasPanelSlot* IconSlot = Cast<UCanvasPanelSlot>(Entry.Icon->Slot))
			{
				IconSlot->SetPosition(FVector2D(
					Data.Position.X * DisplaySize,
					Data.Position.Y * DisplaySize
				));
				IconSlot->SetSize(FVector2D(Size, Size));
			}

			// rotate Self/Teammate markers to show facing direction
			if (Data.Type == EMapMarkerType::Self || Data.Type == EMapMarkerType::Teammate)
			{
				Entry.Icon->SetRenderTransformAngle(Data.Rotation);
			}
			else
			{
				Entry.Icon->SetRenderTransformAngle(0.0f);
			}

			// --- Label (objectives only) ---
			if (Entry.Label)
			{
				if (Data.Type == EMapMarkerType::Objective && !Data.Label.IsEmpty())
				{
					Entry.Label->SetVisibility(ESlateVisibility::HitTestInvisible);
					Entry.Label->SetText(Data.Label);
					Entry.Label->SetColorAndOpacity(FSlateColor(Data.Color));

					if (UCanvasPanelSlot* LabelSlot = Cast<UCanvasPanelSlot>(Entry.Label->Slot))
					{
						LabelSlot->SetPosition(FVector2D(
							Data.Position.X * DisplaySize,
							Data.Position.Y * DisplaySize - Size
						));
					}
				}
				else
				{
					Entry.Label->SetVisibility(ESlateVisibility::Collapsed);
				}
			}
		}

		// hide unused pool entries
		for (int32 i = NewMarkers.Num(); i < MarkerPool.Num(); ++i)
		{
			MarkerPool[i].Icon->SetVisibility(ESlateVisibility::Collapsed);
			if (MarkerPool[i].Label)
			{
				MarkerPool[i].Label->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
	}

	BP_OnMarkersUpdated(NewMarkers);
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void UMapWidget::ApplyRenderTargetToImage()
{
	if (!MapImage || !MapRenderTarget)
	{
		return;
	}

	MapImage->SetBrushResourceObject(MapRenderTarget);
}

void UMapWidget::ApplyMapSize()
{
	if (!MapSizeBox)
	{
		return;
	}

	const float Size = bIsFullMap ? FullMapDisplaySize : MinimapDisplaySize;
	MapSizeBox->SetWidthOverride(Size);
	MapSizeBox->SetHeightOverride(Size);
}

void UMapWidget::EnsureMarkerPoolSize(int32 RequiredSize)
{
	if (!MarkerPanel || !WidgetTree)
	{
		return;
	}

	while (MarkerPool.Num() < RequiredSize)
	{
		FMarkerWidgetEntry Entry;

		// create icon image
		Entry.Icon = WidgetTree->ConstructWidget<UImage>();
		if (UCanvasPanelSlot* IconSlot = MarkerPanel->AddChildToCanvas(Entry.Icon))
		{
			IconSlot->SetAlignment(FVector2D(0.5, 0.5));
			IconSlot->SetAutoSize(false);
			IconSlot->SetSize(FVector2D(PlayerMarkerSize, PlayerMarkerSize));
		}
		Entry.Icon->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));

		// create text label (used for objective letters A, B, C...)
		Entry.Label = WidgetTree->ConstructWidget<UTextBlock>();
		if (UCanvasPanelSlot* LabelSlot = MarkerPanel->AddChildToCanvas(Entry.Label))
		{
			LabelSlot->SetAlignment(FVector2D(0.5, 1.0));
			LabelSlot->SetAutoSize(true);
		}
		FSlateFontInfo FontInfo = Entry.Label->GetFont();
		FontInfo.Size = ObjectiveFontSize;
		Entry.Label->SetFont(FontInfo);
		Entry.Label->SetVisibility(ESlateVisibility::Collapsed);

		MarkerPool.Add(Entry);
	}
}
