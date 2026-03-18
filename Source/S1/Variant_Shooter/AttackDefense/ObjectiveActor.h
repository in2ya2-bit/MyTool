// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AttackDefenseTypes.h"
#include "ObjectiveActor.generated.h"

class USphereComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FObjectiveCapturedDelegate, AObjectiveActor*, CapturedObjective);

UCLASS()
class S1_API AObjectiveActor : public AActor
{
	GENERATED_BODY()

public:

	AObjectiveActor();

	/** Radius of the capture zone */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Capture", meta = (ClampMin = 50, ClampMax = 5000, Units = "cm"))
	float CaptureRadius = 400.0f;

	/** Capture rate per second (1.0 / CaptureRate = seconds to full capture with one attacker) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Capture", meta = (ClampMin = 0.01, ClampMax = 1.0))
	float CaptureRate = 0.1f;

	/** Called on the server when this objective reaches full capture */
	FObjectiveCapturedDelegate OnCaptured;

	/** Sets the objective state (called by GameMode to lock/unlock) */
	void SetObjectiveState(EObjectiveState NewState);

	/** Returns the current state */
	EObjectiveState GetObjectiveState() const { return State; }

	/** Returns capture progress [0, 1] */
	float GetCaptureProgress() const { return CaptureProgress; }

	/** Resets progress to zero and sets state to Locked */
	void ResetObjective();

	/** Sets the sequential index for this objective (A=0, B=1, C=2...) */
	void SetObjectiveIndex(int32 Index) { ObjectiveIndex = Index; }

	/** Returns the sequential index */
	int32 GetObjectiveIndex() const { return ObjectiveIndex; }

protected:

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	USphereComponent* CaptureZone;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Capture")
	EObjectiveState State = EObjectiveState::Locked;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Capture")
	float CaptureProgress = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Capture")
	int32 ObjectiveIndex = 0;

private:

	int32 CountPlayersInZone(ETeamSide Side) const;
	void UpdateCapture(float DeltaTime);
};
