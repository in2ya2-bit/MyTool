// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttackDefensePlayerState.h"
#include "Net/UnrealNetwork.h"

void AAttackDefensePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AAttackDefensePlayerState, TeamSide);
	DOREPLIFETIME(AAttackDefensePlayerState, bIsDead);
	DOREPLIFETIME(AAttackDefensePlayerState, bPendingRespawn);
}
