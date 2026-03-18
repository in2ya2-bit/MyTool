// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AttackDefenseTypes.h"
#include "AttackDefensePlayerState.generated.h"

UCLASS()
class S1_API AAttackDefensePlayerState : public APlayerState
{
	GENERATED_BODY()

public:

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Team")
	ETeamSide TeamSide = ETeamSide::None;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="State")
	bool bIsDead = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="State")
	bool bPendingRespawn = false;
};
