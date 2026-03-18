// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "AttackDefenseTypes.h"
#include "AttackDefenseGameState.generated.h"

UCLASS()
class S1_API AAttackDefenseGameState : public AGameStateBase
{
	GENERATED_BODY()

public:

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// --- Phase ---

	UPROPERTY(ReplicatedUsing = OnRep_Phase, BlueprintReadOnly, Category="Match")
	EMatchPhase CurrentPhase = EMatchPhase::WaitingToStart;

	// --- Round ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Round")
	float RoundTimeRemaining = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Round")
	int32 CurrentRound = 1;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Round")
	ETeamSide AttackingSide = ETeamSide::Attack;

	// --- Objective ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Objective")
	int32 ActiveObjectiveIndex = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Objective")
	float ObjectiveCaptureProgress = 0.0f;

	// --- Tickets ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Tickets")
	int32 AttackTickets = 150;

	// --- Round 1 record (for match winner determination) ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Record")
	bool bRound1AttackSucceeded = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Record")
	float Round1TimeUsed = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Record")
	int32 Round1ObjectivesCaptured = 0;

	// --- Match result ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Result")
	ETeamSide MatchWinner = ETeamSide::None;

protected:

	UFUNCTION()
	void OnRep_Phase();
};
