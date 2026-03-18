// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AttackDefenseTypes.h"
#include "AttackDefenseGameMode.generated.h"

class AAttackDefenseGameState;
class AObjectiveActor;

UCLASS(abstract)
class S1_API AAttackDefenseGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	AAttackDefenseGameMode();

	// --- Tunable rules ---

	/** Duration of each round in seconds */
	UPROPERTY(EditDefaultsOnly, Category="Rules", meta = (ClampMin = 60, Units = "s"))
	float RoundDuration = 600.0f;

	/** Duration of the pre-round phase */
	UPROPERTY(EditDefaultsOnly, Category="Rules", meta = (ClampMin = 5, Units = "s"))
	float PreRoundDuration = 30.0f;

	/** Starting attack tickets per round */
	UPROPERTY(EditDefaultsOnly, Category="Rules", meta = (ClampMin = 1))
	int32 StartingAttackTickets = 150;

	/** Interval between attack team respawn waves */
	UPROPERTY(EditDefaultsOnly, Category="Rules", meta = (ClampMin = 1, Units = "s"))
	float RespawnWaveInterval = 10.0f;

	/** Objective actor classes to spawn, in sequential order */
	UPROPERTY(EditDefaultsOnly, Category="Rules")
	TArray<TSubclassOf<AObjectiveActor>> ObjectiveClasses;

	/** Delay before starting Round 2 after Round 1 ends */
	UPROPERTY(EditDefaultsOnly, Category="Rules", meta = (ClampMin = 1, Units = "s"))
	float RoundTransitionDelay = 5.0f;

protected:

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;

	void StartPreRound();
	void StartRound();
	void EndRound(ETeamSide WinningSide);
	void EndMatch();
	void CheckWinConditions();
	void AssignTeam(APlayerController* PC);

	/** Spawns all ObjectiveClasses into the world and subscribes to their delegates */
	void SpawnObjectives();

	/** Resets all spawned objectives for a new round */
	void ResetObjectives();

	/** Unlocks the next objective in sequence (the one at ActiveObjectiveIndex) */
	void ActivateNextObjective();

	// --- Respawn wave ---

	FTimerHandle RespawnWaveTimer;
	void ProcessRespawnWave();

	/** Respawns a single player at a valid spawn point */
	void RespawnPlayer(APlayerController* PC);

	// --- Objective callback ---

	UFUNCTION()
	void OnObjectiveCaptured(AObjectiveActor* CapturedObj);

	AAttackDefenseGameState* GetGS() const;

private:

	float ElapsedRoundTime = 0.0f;
	TArray<AObjectiveActor*> SpawnedObjectives;

	FTimerHandle PreRoundTimer;
	FTimerHandle RoundTransitionTimer;
};
