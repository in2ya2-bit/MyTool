// Copyright Epic Games, Inc. All Rights Reserved.


#include "ShooterCharacter.h"
#include "ShooterWeapon.h"
#include "EnhancedInputComponent.h"
#include "Components/InputComponent.h"
#include "Components/PawnNoiseEmitterComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "TimerManager.h"
#include "ShooterGameMode.h"

AShooterCharacter::AShooterCharacter()
{
	// create the noise emitter component
	PawnNoiseEmitter = CreateDefaultSubobject<UPawnNoiseEmitterComponent>(TEXT("Pawn Noise Emitter"));

	// create the third person spring arm
	ThirdPersonSpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("Third Person Spring Arm"));
	ThirdPersonSpringArm->SetupAttachment(GetCapsuleComponent());
	ThirdPersonSpringArm->TargetArmLength = 300.0f;
	ThirdPersonSpringArm->SocketOffset = FVector(0.0f, 50.0f, 80.0f);
	ThirdPersonSpringArm->bUsePawnControlRotation = true;
	ThirdPersonSpringArm->bDoCollisionTest = true;

	// create the third person camera (starts deactivated)
	ThirdPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Third Person Camera"));
	ThirdPersonCameraComponent->SetupAttachment(ThirdPersonSpringArm, USpringArmComponent::SocketName);
	ThirdPersonCameraComponent->SetActive(false);

	// character always faces the controller yaw (aiming direction)
	bUseControllerRotationYaw = true;
	GetCharacterMovement()->bOrientRotationToMovement = false;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 600.0f, 0.0f);
}

void AShooterCharacter::BeginPlay()
{
	Super::BeginPlay();

	// reset HP to max
	CurrentHP = MaxHP;

	// ensure character always faces the aiming direction (overrides BP defaults)
	bUseControllerRotationYaw = true;
	GetCharacterMovement()->bOrientRotationToMovement = false;

	// cache the default camera FOV for ADS transitions
	DefaultFOV = GetFirstPersonCameraComponent()->FieldOfView;

	// apply editor-configured third person camera settings
	ThirdPersonSpringArm->TargetArmLength = ThirdPersonArmLength;
	ThirdPersonSpringArm->SocketOffset = ThirdPersonSocketOffset;

	// update the HUD
	OnDamaged.Broadcast(1.0f);
}

void AShooterCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// update aim pitch from controller ([-90, 90] clamped) for AnimBP Aim Offset
	const FRotator ControlRot = GetControlRotation();
	AimPitch = FMath::ClampAngle(ControlRot.Pitch, -90.0f, 90.0f);

	// smooth ADS FOV transition on the active camera
	UCameraComponent* Camera = bIsThirdPerson ? ThirdPersonCameraComponent : GetFirstPersonCameraComponent();
	const float TargetFOV = bIsAimingDownSights ? AimDownSightsFOV : DefaultFOV;

	if (!FMath::IsNearlyEqual(Camera->FieldOfView, TargetFOV, 0.1f))
	{
		Camera->SetFieldOfView(FMath::FInterpTo(Camera->FieldOfView, TargetFOV, DeltaTime, ADSInterpSpeed));
	}
}

void AShooterCharacter::FaceRotation(FRotator NewControlRotation, float DeltaTime)
{
	if (bIsThirdPerson)
	{
		// in 3P: lock yaw to controller so the character always faces the aiming direction
		FRotator FinalRotation(0.0f, NewControlRotation.Yaw, 0.0f);
		Super::FaceRotation(FinalRotation, DeltaTime);
	}
	else
	{
		Super::FaceRotation(NewControlRotation, DeltaTime);
	}
}

void AShooterCharacter::EndPlay(EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	// clear the respawn timer
	GetWorld()->GetTimerManager().ClearTimer(RespawnTimer);
}

void AShooterCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// base class handles move, aim and jump inputs
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Firing
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AShooterCharacter::DoStartFiring);
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AShooterCharacter::DoStopFiring);

		// Switch weapon
		EnhancedInputComponent->BindAction(SwitchWeaponAction, ETriggerEvent::Triggered, this, &AShooterCharacter::DoSwitchWeapon);

		// Aim down sights
		EnhancedInputComponent->BindAction(AimDownSightsAction, ETriggerEvent::Started, this, &AShooterCharacter::DoStartADS);
		EnhancedInputComponent->BindAction(AimDownSightsAction, ETriggerEvent::Completed, this, &AShooterCharacter::DoStopADS);

		// Toggle camera view
		if (ToggleCameraAction)
		{
			EnhancedInputComponent->BindAction(ToggleCameraAction, ETriggerEvent::Started, this, &AShooterCharacter::DoToggleCamera);
		}
	}

}

float AShooterCharacter::TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// ignore if already dead
	if (CurrentHP <= 0.0f)
	{
		return 0.0f;
	}

	// Reduce HP
	CurrentHP -= Damage;

	// Have we depleted HP?
	if (CurrentHP <= 0.0f)
	{
		Die();
	}

	// update the HUD
	OnDamaged.Broadcast(FMath::Max(0.0f, CurrentHP / MaxHP));

	return Damage;
}

void AShooterCharacter::DoAim(float Yaw, float Pitch)
{
	if (!IsDead())
	{
		if (bIsAimingDownSights)
		{
			Yaw *= ADSSensitivityMultiplier;
			Pitch *= ADSSensitivityMultiplier;
		}
		Super::DoAim(Yaw, Pitch);
	}
}

void AShooterCharacter::DoMove(float Right, float Forward)
{
	if (!IsDead() && GetController())
	{
		// use controller rotation so movement always matches the look direction
		const FRotator ControlRot(0.0f, GetControlRotation().Yaw, 0.0f);
		const FVector ForwardDir = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::X);
		const FVector RightDir = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::Y);

		AddMovementInput(ForwardDir, Forward);
		AddMovementInput(RightDir, Right);
	}
}

void AShooterCharacter::DoJumpStart()
{
	// only route inputs if the character is not dead
	if (!IsDead())
	{
		Super::DoJumpStart();
	}
}

void AShooterCharacter::DoJumpEnd()
{
	// only route inputs if the character is not dead
	if (!IsDead())
	{
		Super::DoJumpEnd();
	}
}

void AShooterCharacter::DoStartFiring()
{
	// fire the current weapon
	if (CurrentWeapon && !IsDead())
	{
		CurrentWeapon->StartFiring();
	}
}

void AShooterCharacter::DoStopFiring()
{
	// stop firing the current weapon
	if (CurrentWeapon && !IsDead())
	{
		CurrentWeapon->StopFiring();
	}
}

void AShooterCharacter::DoSwitchWeapon()
{
	// ensure we have at least two weapons two switch between
	if (OwnedWeapons.Num() > 1 && !IsDead())
	{
		// deactivate the old weapon
		CurrentWeapon->DeactivateWeapon();

		// find the index of the current weapon in the owned list
		int32 WeaponIndex = OwnedWeapons.Find(CurrentWeapon);

		// is this the last weapon?
		if (WeaponIndex == OwnedWeapons.Num() - 1)
		{
			// loop back to the beginning of the array
			WeaponIndex = 0;
		}
		else {
			// select the next weapon index
			++WeaponIndex;
		}

		// set the new weapon as current
		CurrentWeapon = OwnedWeapons[WeaponIndex];

		// activate the new weapon
		CurrentWeapon->ActivateWeapon();
	}
}

void AShooterCharacter::AttachWeaponMeshes(AShooterWeapon* Weapon)
{
	const FAttachmentTransformRules AttachmentRule(EAttachmentRule::SnapToTarget, false);

	// attach the weapon actor
	Weapon->AttachToActor(this, AttachmentRule);

	// attach the weapon meshes
	Weapon->GetFirstPersonMesh()->AttachToComponent(GetFirstPersonMesh(), AttachmentRule, FirstPersonWeaponSocket);
	Weapon->GetThirdPersonMesh()->AttachToComponent(GetMesh(), AttachmentRule, FirstPersonWeaponSocket);
	
}

void AShooterCharacter::PlayFiringMontage(UAnimMontage* Montage)
{
	// stub
}

void AShooterCharacter::AddWeaponRecoil(float Recoil)
{
	// apply the recoil as pitch input
	AddControllerPitchInput(Recoil);
}

void AShooterCharacter::UpdateWeaponHUD(int32 CurrentAmmo, int32 MagazineSize)
{
	OnBulletCountUpdated.Broadcast(MagazineSize, CurrentAmmo);
}

FVector AShooterCharacter::GetWeaponTargetLocation()
{
	// use the active camera for the aim trace
	UCameraComponent* ActiveCamera = bIsThirdPerson ? ThirdPersonCameraComponent : GetFirstPersonCameraComponent();

	FHitResult OutHit;

	const FVector Start = ActiveCamera->GetComponentLocation();
	const FVector End = Start + (ActiveCamera->GetForwardVector() * MaxAimDistance);

	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(this);

	GetWorld()->LineTraceSingleByChannel(OutHit, Start, End, ECC_Visibility, QueryParams);

	return OutHit.bBlockingHit ? OutHit.ImpactPoint : OutHit.TraceEnd;
}

void AShooterCharacter::AddWeaponClass(const TSubclassOf<AShooterWeapon>& WeaponClass)
{
	// do we already own this weapon?
	AShooterWeapon* OwnedWeapon = FindWeaponOfType(WeaponClass);

	if (!OwnedWeapon)
	{
		// spawn the new weapon
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		SpawnParams.Instigator = this;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.TransformScaleMethod = ESpawnActorScaleMethod::MultiplyWithRoot;

		AShooterWeapon* AddedWeapon = GetWorld()->SpawnActor<AShooterWeapon>(WeaponClass, GetActorTransform(), SpawnParams);

		if (AddedWeapon)
		{
			// add the weapon to the owned list
			OwnedWeapons.Add(AddedWeapon);

			// if we have an existing weapon, deactivate it
			if (CurrentWeapon)
			{
				CurrentWeapon->DeactivateWeapon();
			}

			// switch to the new weapon
			CurrentWeapon = AddedWeapon;
			CurrentWeapon->ActivateWeapon();
		}
	}
	else if (OwnedWeapon != CurrentWeapon)
	{
		// already own this weapon but it's not active — switch to it and refill ammo
		CurrentWeapon->DeactivateWeapon();
		OwnedWeapon->RefillAmmo();
		CurrentWeapon = OwnedWeapon;
		CurrentWeapon->ActivateWeapon();
	}
	else
	{
		// already holding this weapon — just refill ammo
		OwnedWeapon->RefillAmmo();
		OnBulletCountUpdated.Broadcast(OwnedWeapon->GetMagazineSize(), OwnedWeapon->GetBulletCount());
	}
}

void AShooterCharacter::OnWeaponActivated(AShooterWeapon* Weapon)
{
	// update the bullet counter
	OnBulletCountUpdated.Broadcast(Weapon->GetMagazineSize(), Weapon->GetBulletCount());

	// set the character mesh AnimInstances
	GetFirstPersonMesh()->SetAnimInstanceClass(Weapon->GetFirstPersonAnimInstanceClass());
	GetMesh()->SetAnimInstanceClass(Weapon->GetThirdPersonAnimInstanceClass());

	// match weapon mesh visibility to current camera mode
	ApplyWeaponMeshVisibility(Weapon);

	OnWeaponEquipped.Broadcast(true);
}

void AShooterCharacter::OnWeaponDeactivated(AShooterWeapon* Weapon)
{
	if (bIsAimingDownSights)
	{
		bIsAimingDownSights = false;
		OnADSStateChanged.Broadcast(false);
	}

	OnWeaponEquipped.Broadcast(false);
}

void AShooterCharacter::OnSemiWeaponRefire()
{
	// unused
}

AShooterWeapon* AShooterCharacter::FindWeaponOfType(TSubclassOf<AShooterWeapon> WeaponClass) const
{
	// check each owned weapon
	for (AShooterWeapon* Weapon : OwnedWeapons)
	{
		if (Weapon->IsA(WeaponClass))
		{
			return Weapon;
		}
	}

	// weapon not found
	return nullptr;

}

void AShooterCharacter::ApplyWeaponMeshVisibility(AShooterWeapon* Weapon) const
{
	if (!IsValid(Weapon))
	{
		return;
	}

	if (bIsThirdPerson)
	{
		Weapon->GetFirstPersonMesh()->SetVisibility(false);
		Weapon->GetThirdPersonMesh()->SetOwnerNoSee(false);
		Weapon->GetThirdPersonMesh()->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::None);
	}
	else
	{
		Weapon->GetFirstPersonMesh()->SetVisibility(true);
		Weapon->GetThirdPersonMesh()->SetOwnerNoSee(true);
		Weapon->GetThirdPersonMesh()->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::WorldSpaceRepresentation);
	}
}

void AShooterCharacter::DoStartADS()
{
	if (!IsDead() && CurrentWeapon)
	{
		bIsAimingDownSights = true;
		OnADSStateChanged.Broadcast(true);
	}
}

void AShooterCharacter::DoStopADS()
{
	bIsAimingDownSights = false;
	OnADSStateChanged.Broadcast(false);
}

void AShooterCharacter::DoToggleCamera()
{
	if (IsDead())
	{
		return;
	}

	bIsThirdPerson = !bIsThirdPerson;

	// sync the new camera's FOV to the current state before switching
	const float CurrentFOV = bIsAimingDownSights ? AimDownSightsFOV : DefaultFOV;

	if (bIsThirdPerson)
	{
		ThirdPersonCameraComponent->SetFieldOfView(GetFirstPersonCameraComponent()->FieldOfView);

		// switch to third person camera
		GetFirstPersonCameraComponent()->SetActive(false);
		ThirdPersonCameraComponent->SetActive(true);

		// make the third person mesh render normally so the owner can see it
		GetMesh()->SetOwnerNoSee(false);
		GetMesh()->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::None);

		// hide first person mesh
		GetFirstPersonMesh()->SetVisibility(false);
	}
	else
	{
		GetFirstPersonCameraComponent()->SetFieldOfView(ThirdPersonCameraComponent->FieldOfView);

		// switch to first person camera
		ThirdPersonCameraComponent->SetActive(false);
		GetFirstPersonCameraComponent()->SetActive(true);

		// restore first person rendering: TP mesh hidden from owner, FP mesh visible
		GetMesh()->SetOwnerNoSee(true);
		GetMesh()->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::WorldSpaceRepresentation);

		GetFirstPersonMesh()->SetVisibility(true);
	}

	// update weapon mesh visibility for the current weapon
	ApplyWeaponMeshVisibility(CurrentWeapon);
}

void AShooterCharacter::Die()
{
	// reset to first person on death
	if (bIsThirdPerson)
	{
		bIsThirdPerson = false;
		ThirdPersonCameraComponent->SetActive(false);
		GetFirstPersonCameraComponent()->SetActive(true);
		GetMesh()->SetOwnerNoSee(true);
		GetMesh()->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::WorldSpaceRepresentation);
		GetFirstPersonMesh()->SetVisibility(true);
	}

	// exit ADS on death
	if (bIsAimingDownSights)
	{
		bIsAimingDownSights = false;
		OnADSStateChanged.Broadcast(false);
	}

	// deactivate the weapon
	if (IsValid(CurrentWeapon))
	{
		CurrentWeapon->DeactivateWeapon();
	}

	// increment the team score
	if (AShooterGameMode* GM = Cast<AShooterGameMode>(GetWorld()->GetAuthGameMode()))
	{
		GM->IncrementTeamScore(TeamByte);
	}

	// grant the death tag to the character
	Tags.Add(DeathTag);
		
	// stop character movement
	GetCharacterMovement()->StopMovementImmediately();

	// disable controls
	DisableInput(nullptr);

	// reset the bullet counter UI
	OnBulletCountUpdated.Broadcast(0, 0);

	// call the BP handler
	BP_OnDeath();

	// schedule character respawn
	GetWorld()->GetTimerManager().SetTimer(RespawnTimer, this, &AShooterCharacter::OnRespawn, RespawnTime, false);
}

void AShooterCharacter::OnRespawn()
{
	// destroy the character to force the PC to respawn
	Destroy();
}

bool AShooterCharacter::IsDead() const
{
	// the character is dead if their current HP drops to zero
	return CurrentHP <= 0.0f;
}
