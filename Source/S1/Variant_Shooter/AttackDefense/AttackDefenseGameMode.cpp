// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttackDefenseGameMode.h"
#include "AttackDefenseGameState.h"
#include "AttackDefensePlayerState.h"
#include "ObjectiveActor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "S1.h"

AAttackDefenseGameMode::AAttackDefenseGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	GameStateClass = AAttackDefenseGameState::StaticClass();
	PlayerStateClass = AAttackDefensePlayerState::StaticClass();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AAttackDefenseGameMode::BeginPlay()
{
	Super::BeginPlay();
	SpawnObjectives();
	StartPreRound();
}

void AAttackDefenseGameMode::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	if (GS->CurrentPhase == EMatchPhase::PreRound)
	{
		GS->RoundTimeRemaining -= DeltaTime;
		if (GS->RoundTimeRemaining <= 0.0f)
		{
			StartRound();
		}
		return;
	}

	if (GS->CurrentPhase != EMatchPhase::RoundInProgress)
	{
		return;
	}

	GS->RoundTimeRemaining -= DeltaTime;
	ElapsedRoundTime += DeltaTime;

	// sync capture progress to GameState for HUD
	if (GS->ActiveObjectiveIndex < SpawnedObjectives.Num())
	{
		GS->ObjectiveCaptureProgress = SpawnedObjectives[GS->ActiveObjectiveIndex]->GetCaptureProgress();
	}

	if (GS->RoundTimeRemaining <= 0.0f)
	{
		const ETeamSide DefendSide = (GS->AttackingSide == ETeamSide::Attack)
			? ETeamSide::Defend
			: ETeamSide::Attack;
		EndRound(DefendSide);
		return;
	}

	CheckWinConditions();
}

void AAttackDefenseGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	AssignTeam(NewPlayer);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void AAttackDefenseGameMode::StartPreRound()
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	GS->CurrentPhase = EMatchPhase::PreRound;
	GS->RoundTimeRemaining = PreRoundDuration;
	GS->AttackTickets = StartingAttackTickets;
	GS->ActiveObjectiveIndex = 0;
	GS->ObjectiveCaptureProgress = 0.0f;
	ElapsedRoundTime = 0.0f;

	ResetObjectives();

	UE_LOG(LogS1, Log, TEXT("AttackDefense: PreRound started (Round %d)"), GS->CurrentRound);
}

void AAttackDefenseGameMode::StartRound()
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	GS->CurrentPhase = EMatchPhase::RoundInProgress;
	GS->RoundTimeRemaining = RoundDuration;
	ElapsedRoundTime = 0.0f;

	ActivateNextObjective();

	GetWorldTimerManager().SetTimer(
		RespawnWaveTimer,
		this,
		&AAttackDefenseGameMode::ProcessRespawnWave,
		RespawnWaveInterval,
		true
	);

	UE_LOG(LogS1, Log, TEXT("AttackDefense: Round %d started"), GS->CurrentRound);
}

void AAttackDefenseGameMode::EndRound(ETeamSide WinningSide)
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	GS->CurrentPhase = EMatchPhase::RoundEnd;
	GetWorldTimerManager().ClearTimer(RespawnWaveTimer);

	UE_LOG(LogS1, Log, TEXT("AttackDefense: Round %d ended — Winner: %s"),
		GS->CurrentRound,
		WinningSide == ETeamSide::Attack ? TEXT("Attack") : TEXT("Defend"));

	if (GS->CurrentRound == 1)
	{
		GS->bRound1AttackSucceeded = (WinningSide == GS->AttackingSide);
		GS->Round1TimeUsed = ElapsedRoundTime;
		GS->Round1ObjectivesCaptured = GS->ActiveObjectiveIndex;

		// swap attack/defend sides for round 2
		GS->AttackingSide = (GS->AttackingSide == ETeamSide::Attack)
			? ETeamSide::Defend
			: ETeamSide::Attack;
		GS->CurrentRound = 2;

		// swap all players' team sides
		for (APlayerState* PS : GS->PlayerArray)
		{
			if (AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PS))
			{
				APS->TeamSide = (APS->TeamSide == ETeamSide::Attack)
					? ETeamSide::Defend
					: ETeamSide::Attack;
			}
		}

		GetWorldTimerManager().SetTimer(
			RoundTransitionTimer,
			this,
			&AAttackDefenseGameMode::StartPreRound,
			RoundTransitionDelay,
			false
		);
	}
	else
	{
		EndMatch();
	}
}

void AAttackDefenseGameMode::EndMatch()
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	GS->CurrentPhase = EMatchPhase::MatchEnd;
	GetWorldTimerManager().ClearTimer(RespawnWaveTimer);

	const bool bR1 = GS->bRound1AttackSucceeded;
	const bool bR2 = (GS->ActiveObjectiveIndex >= SpawnedObjectives.Num());

	ETeamSide Winner = ETeamSide::None;

	if (bR1 && bR2)
	{
		// both sides succeeded as attacker — faster time wins
		// Round 1 attacker was the original Attack team
		Winner = (GS->Round1TimeUsed <= ElapsedRoundTime)
			? ETeamSide::Attack
			: ETeamSide::Defend;
	}
	else if (bR1)
	{
		Winner = ETeamSide::Attack;
	}
	else if (bR2)
	{
		Winner = ETeamSide::Defend;
	}
	else
	{
		// both failed — team that captured more objectives wins
		const int32 R2Captured = GS->ActiveObjectiveIndex;
		if (GS->Round1ObjectivesCaptured > R2Captured)
		{
			Winner = ETeamSide::Attack;
		}
		else if (R2Captured > GS->Round1ObjectivesCaptured)
		{
			Winner = ETeamSide::Defend;
		}
		// else remains None (draw)
	}

	GS->MatchWinner = Winner;

	UE_LOG(LogS1, Log, TEXT("AttackDefense: Match ended — Winner: %s"),
		Winner == ETeamSide::Attack ? TEXT("Attack (Original)") :
		Winner == ETeamSide::Defend ? TEXT("Defend (Original)") : TEXT("Draw"));
}

void AAttackDefenseGameMode::CheckWinConditions()
{
	AAttackDefenseGameState* GS = GetGS();

	// all objectives captured
	if (GS->ActiveObjectiveIndex >= SpawnedObjectives.Num())
	{
		EndRound(GS->AttackingSide);
		return;
	}

	// attack tickets depleted
	if (GS->AttackTickets <= 0)
	{
		// also check if any attackers are still alive
		bool bAnyAttackerAlive = false;
		for (APlayerState* PS : GS->PlayerArray)
		{
			if (const AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PS))
			{
				if (APS->TeamSide == GS->AttackingSide && !APS->bIsDead)
				{
					bAnyAttackerAlive = true;
					break;
				}
			}
		}

		if (!bAnyAttackerAlive)
		{
			const ETeamSide DefendSide = (GS->AttackingSide == ETeamSide::Attack)
				? ETeamSide::Defend
				: ETeamSide::Attack;
			EndRound(DefendSide);
		}
	}
}

// ---------------------------------------------------------------------------
// Team assignment
// ---------------------------------------------------------------------------

void AAttackDefenseGameMode::AssignTeam(APlayerController* PC)
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS || !PC)
	{
		return;
	}

	int32 AttackCount = 0;
	int32 DefendCount = 0;

	for (APlayerState* PS : GS->PlayerArray)
	{
		if (const AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PS))
		{
			if (APS->TeamSide == ETeamSide::Attack) { ++AttackCount; }
			else if (APS->TeamSide == ETeamSide::Defend) { ++DefendCount; }
		}
	}

	if (AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PC->PlayerState))
	{
		APS->TeamSide = (AttackCount <= DefendCount)
			? ETeamSide::Attack
			: ETeamSide::Defend;

		UE_LOG(LogS1, Log, TEXT("AttackDefense: %s assigned to %s"),
			*PC->GetName(),
			APS->TeamSide == ETeamSide::Attack ? TEXT("Attack") : TEXT("Defend"));
	}
}

// ---------------------------------------------------------------------------
// Objectives
// ---------------------------------------------------------------------------

void AAttackDefenseGameMode::SpawnObjectives()
{
	for (int32 i = 0; i < ObjectiveClasses.Num(); ++i)
	{
		if (!ObjectiveClasses[i])
		{
			continue;
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

		AObjectiveActor* Obj = GetWorld()->SpawnActor<AObjectiveActor>(ObjectiveClasses[i], FTransform::Identity, Params);
		if (Obj)
		{
			Obj->SetObjectiveIndex(i);
			Obj->OnCaptured.AddDynamic(this, &AAttackDefenseGameMode::OnObjectiveCaptured);
			SpawnedObjectives.Add(Obj);
		}
	}

	UE_LOG(LogS1, Log, TEXT("AttackDefense: Spawned %d objectives"), SpawnedObjectives.Num());
}

void AAttackDefenseGameMode::ResetObjectives()
{
	for (AObjectiveActor* Obj : SpawnedObjectives)
	{
		if (IsValid(Obj))
		{
			Obj->ResetObjective();
		}
	}
}

void AAttackDefenseGameMode::ActivateNextObjective()
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	const int32 Idx = GS->ActiveObjectiveIndex;
	if (Idx < SpawnedObjectives.Num() && IsValid(SpawnedObjectives[Idx]))
	{
		SpawnedObjectives[Idx]->SetObjectiveState(EObjectiveState::Active);
		UE_LOG(LogS1, Log, TEXT("AttackDefense: Objective %d is now Active"), Idx);
	}
}

void AAttackDefenseGameMode::OnObjectiveCaptured(AObjectiveActor* CapturedObj)
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS)
	{
		return;
	}

	UE_LOG(LogS1, Log, TEXT("AttackDefense: Objective %d captured"), GS->ActiveObjectiveIndex);

	GS->ActiveObjectiveIndex++;
	GS->ObjectiveCaptureProgress = 0.0f;

	if (GS->ActiveObjectiveIndex < SpawnedObjectives.Num())
	{
		ActivateNextObjective();
	}
	// CheckWinConditions in Tick will detect all objectives captured
}

// ---------------------------------------------------------------------------
// Respawn
// ---------------------------------------------------------------------------

void AAttackDefenseGameMode::ProcessRespawnWave()
{
	AAttackDefenseGameState* GS = GetGS();
	if (!GS || GS->CurrentPhase != EMatchPhase::RoundInProgress)
	{
		return;
	}

	for (APlayerState* PS : GS->PlayerArray)
	{
		AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PS);
		if (!APS || !APS->bPendingRespawn)
		{
			continue;
		}

		if (APS->TeamSide == GS->AttackingSide)
		{
			if (GS->AttackTickets > 0)
			{
				GS->AttackTickets--;
				RespawnPlayer(Cast<APlayerController>(APS->GetOwner()));
				APS->bPendingRespawn = false;
				APS->bIsDead = false;
			}
		}
		else
		{
			RespawnPlayer(Cast<APlayerController>(APS->GetOwner()));
			APS->bPendingRespawn = false;
			APS->bIsDead = false;
		}
	}
}

void AAttackDefenseGameMode::RespawnPlayer(APlayerController* PC)
{
	if (!PC)
	{
		return;
	}

	AActor* StartSpot = FindPlayerStart(PC);
	if (!StartSpot)
	{
		UE_LOG(LogS1, Warning, TEXT("AttackDefense: No valid spawn point found for %s"), *PC->GetName());
		return;
	}

	RestartPlayerAtPlayerStart(PC, StartSpot);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AAttackDefenseGameState* AAttackDefenseGameMode::GetGS() const
{
	return GetGameState<AAttackDefenseGameState>();
}
