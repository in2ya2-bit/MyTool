// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ShooterCrosshairUI.generated.h"

/**
 *  Crosshair UI widget displayed at the center of the screen during ADS
 *  Blueprint is responsible for the visual layout (e.g. a red dot image)
 */
UCLASS(abstract)
class S1_API UShooterCrosshairUI : public UUserWidget
{
	GENERATED_BODY()
	
public:

	/** Allows Blueprint to show or hide the crosshair with custom animations */
	UFUNCTION(BlueprintImplementableEvent, Category="Shooter", meta=(DisplayName = "Set Crosshair Visible"))
	void BP_SetCrosshairVisible(bool bVisible);
};
