// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ShooterWeaponData.generated.h"

class UCameraShakeBase;

/**
 *  Data asset that defines per-weapon tuning parameters
 *  Assign to a ShooterWeapon to override its default values
 */
UCLASS(BlueprintType)
class S1_API UShooterWeaponData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

	/** Amount of firing recoil applied to the camera pitch */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Recoil", meta = (ClampMin = 0, ClampMax = 100))
	float FiringRecoil = 1.0f;

	/** Maximum random horizontal recoil applied per shot */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Recoil", meta = (ClampMin = 0, ClampMax = 50))
	float HorizontalRecoil = 0.0f;

	/** If true, this weapon fires automatically while the trigger is held */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Fire Rate")
	bool bFullAuto = false;

	/** Time between shots (lower = faster fire rate) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Fire Rate", meta = (ClampMin = 0.01, ClampMax = 5, Units = "s"))
	float RefireRate = 0.5f;

	/** Number of bullets per magazine */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Ammo", meta = (ClampMin = 1, ClampMax = 200))
	int32 MagazineSize = 10;

	/** Cone half-angle for aiming variance (spread) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Accuracy", meta = (ClampMin = 0, ClampMax = 90, Units = "Degrees"))
	float AimVariance = 0.0f;

	/** Speed of projectiles fired by this weapon (0 = use projectile default) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Projectile", meta = (ClampMin = 0, ClampMax = 100000, Units = "cm/s"))
	float ProjectileSpeed = 0.0f;

	/** Camera shake class to play on each shot */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Camera Shake")
	TSubclassOf<UCameraShakeBase> FireCameraShakeClass;

	/** Camera shake intensity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Camera Shake", meta = (ClampMin = 0, ClampMax = 10))
	float CameraShakeScale = 1.0f;
};
