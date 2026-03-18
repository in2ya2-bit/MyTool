// Copyright Epic Games, Inc. All Rights Reserved.

#include "ObjectiveActor.h"
#include "AttackDefensePlayerState.h"
#include "Components/SphereComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/Character.h"

AObjectiveActor::AObjectiveActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	CaptureZone = CreateDefaultSubobject<USphereComponent>(TEXT("CaptureZone"));
	CaptureZone->InitSphereRadius(CaptureRadius);
	CaptureZone->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	CaptureZone->SetGenerateOverlapEvents(true);
	RootComponent = CaptureZone;
}

void AObjectiveActor::BeginPlay()
{
	Super::BeginPlay();
	CaptureZone->SetSphereRadius(CaptureRadius);
}

void AObjectiveActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (HasAuthority())
	{
		UpdateCapture(DeltaTime);
	}
}

void AObjectiveActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AObjectiveActor, State);
	DOREPLIFETIME(AObjectiveActor, CaptureProgress);
	DOREPLIFETIME(AObjectiveActor, ObjectiveIndex);
}

void AObjectiveActor::SetObjectiveState(EObjectiveState NewState)
{
	State = NewState;
}

void AObjectiveActor::ResetObjective()
{
	CaptureProgress = 0.0f;
	State = EObjectiveState::Locked;
}

void AObjectiveActor::UpdateCapture(float DeltaTime)
{
	if (State == EObjectiveState::Locked || State == EObjectiveState::Captured)
	{
		return;
	}

	const int32 Attackers = CountPlayersInZone(ETeamSide::Attack);
	const int32 Defenders = CountPlayersInZone(ETeamSide::Defend);

	if (Attackers > 0 && Defenders > 0)
	{
		State = EObjectiveState::Contested;
		return;
	}

	State = EObjectiveState::Active;

	if (Attackers > 0)
	{
		CaptureProgress = FMath::Min(1.0f, CaptureProgress + CaptureRate * DeltaTime);

		if (CaptureProgress >= 1.0f)
		{
			State = EObjectiveState::Captured;
			OnCaptured.Broadcast(this);
		}
	}
}

int32 AObjectiveActor::CountPlayersInZone(ETeamSide Side) const
{
	TArray<AActor*> OverlappingActors;
	CaptureZone->GetOverlappingActors(OverlappingActors, ACharacter::StaticClass());

	int32 Count = 0;
	for (AActor* Actor : OverlappingActors)
	{
		if (const APawn* Pawn = Cast<APawn>(Actor))
		{
			if (const AAttackDefensePlayerState* PS = Pawn->GetPlayerState<AAttackDefensePlayerState>())
			{
				if (PS->TeamSide == Side && !PS->bIsDead)
				{
					++Count;
				}
			}
		}
	}
	return Count;
}
