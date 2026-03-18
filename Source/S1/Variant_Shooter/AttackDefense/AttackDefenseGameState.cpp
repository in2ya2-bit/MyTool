// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttackDefenseGameState.h"
#include "Net/UnrealNetwork.h"

void AAttackDefenseGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AAttackDefenseGameState, CurrentPhase);
	DOREPLIFETIME(AAttackDefenseGameState, RoundTimeRemaining);
	DOREPLIFETIME(AAttackDefenseGameState, CurrentRound);
	DOREPLIFETIME(AAttackDefenseGameState, AttackingSide);
	DOREPLIFETIME(AAttackDefenseGameState, ActiveObjectiveIndex);
	DOREPLIFETIME(AAttackDefenseGameState, ObjectiveCaptureProgress);
	DOREPLIFETIME(AAttackDefenseGameState, AttackTickets);
	DOREPLIFETIME(AAttackDefenseGameState, bRound1AttackSucceeded);
	DOREPLIFETIME(AAttackDefenseGameState, Round1TimeUsed);
	DOREPLIFETIME(AAttackDefenseGameState, Round1ObjectivesCaptured);
	DOREPLIFETIME(AAttackDefenseGameState, MatchWinner);
}

void AAttackDefenseGameState::OnRep_Phase()
{
	// clients can react to phase transitions here (e.g. UI refresh)
}
