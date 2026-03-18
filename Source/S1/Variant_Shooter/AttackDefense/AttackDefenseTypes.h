// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttackDefenseTypes.generated.h"

UENUM(BlueprintType)
enum class EMatchPhase : uint8
{
	WaitingToStart,
	PreRound,
	RoundInProgress,
	RoundEnd,
	MatchEnd
};

UENUM(BlueprintType)
enum class EObjectiveState : uint8
{
	Locked,
	Active,
	Contested,
	Captured
};

UENUM(BlueprintType)
enum class ETeamSide : uint8
{
	None,
	Attack,
	Defend
};
