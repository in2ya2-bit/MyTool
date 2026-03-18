// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MapWidget.generated.h"

class UTextureRenderTarget2D;
class UImage;
class UCanvasPanel;
class UTextBlock;
class USizeBox;

/**
 *  Marker type for map display elements
 */
UENUM(BlueprintType)
enum class EMapMarkerType : uint8
{
	Self,
	Teammate,
	Objective
};

/**
 *  Data describing a single marker on the map
 *  Position is in normalized [0,1] UV space relative to the map image
 */
USTRUCT(BlueprintType)
struct FMapMarkerData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Map")
	EMapMarkerType Type = EMapMarkerType::Self;

	/** Normalized position on the map image [0,1] */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	FVector2D Position = FVector2D(0.5, 0.5);

	/** Yaw rotation in degrees */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	float Rotation = 0.0f;

	/** Display label (e.g. "A", "B", "C" for objectives) */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	FText Label;

	/** Progress value [0,1] for capture objectives */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	float Progress = 0.0f;

	/** Color hint for the marker (BP uses this for rendering) */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	FLinearColor Color = FLinearColor::White;

	/** True if this objective is currently active or contested */
	UPROPERTY(BlueprintReadOnly, Category="Map")
	bool bIsActive = false;
};

/**
 *  Base widget for minimap and full-map display.
 *
 *  HOW TO SET UP in the Widget Blueprint Designer:
 *    1. Add a SizeBox named "MapSizeBox"  (controls minimap / fullmap size)
 *    2. Inside it, add a CanvasPanel
 *    3. Inside that CanvasPanel, add:
 *       - an Image named "MapImage"         (anchored to fill, displays render target)
 *       - a CanvasPanel named "MarkerPanel" (anchored to fill, holds marker icons)
 *
 *  C++ automatically:
 *    - Applies the render target to MapImage
 *    - Creates / positions / colors marker icons inside MarkerPanel
 *    - Resizes MapSizeBox when toggling minimap <-> fullmap
 *
 *  BP_* events still fire for additional customization if needed.
 */
UCLASS(abstract)
class S1_API UMapWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	// ---- Render target (set by the owning controller) ----

	UPROPERTY(BlueprintReadOnly, Category="Map")
	TObjectPtr<UTextureRenderTarget2D> MapRenderTarget;

	UPROPERTY(BlueprintReadOnly, Category="Map")
	bool bIsFullMap = false;

	// ---- Layout settings (set in BP Class Defaults) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Layout")
	float MinimapDisplaySize = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Layout")
	float FullMapDisplaySize = 600.0f;

	// ---- Marker sizes ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Markers")
	float PlayerMarkerSize = 14.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Markers")
	float TeammateMarkerSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Markers")
	float ObjectiveMarkerSize = 22.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Map|Markers")
	int32 ObjectiveFontSize = 11;

	// ---- Controller interface ----

	void SetMapMode(bool bFullMap);
	void UpdateMarkers(const TArray<FMapMarkerData>& NewMarkers);
	void SetRenderTarget(UTextureRenderTarget2D* InTarget);

	// ---- BP events (for additional customization) ----

	UFUNCTION(BlueprintImplementableEvent, Category="Map", meta=(DisplayName="On Map Mode Changed"))
	void BP_OnMapModeChanged(bool bFullMap);

	UFUNCTION(BlueprintImplementableEvent, Category="Map", meta=(DisplayName="On Markers Updated"))
	void BP_OnMarkersUpdated(const TArray<FMapMarkerData>& Markers);

	UFUNCTION(BlueprintImplementableEvent, Category="Map", meta=(DisplayName="On Render Target Set"))
	void BP_OnRenderTargetSet(UTextureRenderTarget2D* Target);

protected:

	virtual void NativeConstruct() override;

	// ---- BindWidgetOptional: add these in BP Designer with matching names ----

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	UImage* MapImage = nullptr;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	UCanvasPanel* MarkerPanel = nullptr;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	USizeBox* MapSizeBox = nullptr;

private:

	struct FMarkerWidgetEntry
	{
		UImage* Icon = nullptr;
		UTextBlock* Label = nullptr;
	};

	TArray<FMarkerWidgetEntry> MarkerPool;

	void EnsureMarkerPoolSize(int32 RequiredSize);
	void ApplyRenderTargetToImage();
	void ApplyMapSize();
};
