// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShooterPlayerController.h"
#include "AttackDefensePlayerController.generated.h"

class UMapWidget;
class UInputAction;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class AObjectiveActor;

/**
 *  PlayerController for the Attack/Defense game mode
 *  Extends the base shooter controller with minimap and full-map functionality
 *  Manages a top-down SceneCaptureComponent2D for real-time map rendering
 */
UCLASS(abstract)
class S1_API AAttackDefensePlayerController : public AShooterPlayerController
{
	GENERATED_BODY()

public:

	AAttackDefensePlayerController();

protected:

	// --- Map Widget ---

	/** Widget class to use for the minimap / full map display */
	UPROPERTY(EditAnywhere, Category="Map|Widget")
	TSubclassOf<UMapWidget> MapWidgetClass;

	UPROPERTY()
	TObjectPtr<UMapWidget> MapWidget;

	// --- Map Input ---

	/** Input action bound to the M key for toggling the full map */
	UPROPERTY(EditAnywhere, Category="Map|Input")
	TObjectPtr<UInputAction> ToggleMapAction;

	// --- Map Capture Settings ---

	/** Height of the top-down capture camera above the player */
	UPROPERTY(EditAnywhere, Category="Map|Capture", meta = (ClampMin = 500, Units = "cm"))
	float CaptureHeight = 8000.0f;

	/** Orthographic width for the minimap capture (area around the player) */
	UPROPERTY(EditAnywhere, Category="Map|Capture", meta = (ClampMin = 500, Units = "cm"))
	float MinimapOrthoWidth = 5000.0f;

	/** Orthographic width for the full map capture (entire level) */
	UPROPERTY(EditAnywhere, Category="Map|Capture", meta = (ClampMin = 1000, Units = "cm"))
	float FullMapOrthoWidth = 50000.0f;

	/** World position to center the full map capture on */
	UPROPERTY(EditAnywhere, Category="Map|Capture")
	FVector FullMapCenter = FVector::ZeroVector;

	/** Resolution of the map render target (width = height) */
	UPROPERTY(EditAnywhere, Category="Map|Capture", meta = (ClampMin = 64, ClampMax = 2048))
	int32 MapResolution = 512;

	// --- Internal ---

	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> MapRenderTarget;

	/** Cached objective actors, populated lazily */
	UPROPERTY()
	TArray<TObjectPtr<AObjectiveActor>> CachedObjectives;

	bool bIsFullMap = false;

protected:

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupInputComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Creates the render target and scene capture component */
	void InitializeMapCapture();

	/** Positions the capture camera based on the current map mode */
	void UpdateMapCapture();

	/** Collects all marker data (player, teammates, objectives) */
	void GatherMapMarkers(TArray<struct FMapMarkerData>& OutMarkers) const;

	/** Converts a world position to [0,1] UV on the current capture view */
	FVector2D WorldToMapPosition(const FVector& WorldPos) const;

	/** Toggles between minimap and full map */
	void ToggleMap();

	/** Finds and caches all ObjectiveActors in the level */
	void EnsureObjectivesCached();
};
