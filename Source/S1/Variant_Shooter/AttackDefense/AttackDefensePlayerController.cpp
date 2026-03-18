// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttackDefensePlayerController.h"
#include "AttackDefenseGameState.h"
#include "AttackDefensePlayerState.h"
#include "ObjectiveActor.h"
#include "MapWidget.h"
#include "EnhancedInputComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/GameplayStatics.h"
#include "S1.h"

AAttackDefensePlayerController::AAttackDefensePlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AAttackDefensePlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalPlayerController())
	{
		return;
	}

	InitializeMapCapture();

	if (MapWidgetClass)
	{
		MapWidget = CreateWidget<UMapWidget>(this, MapWidgetClass);
		if (MapWidget)
		{
			MapWidget->AddToPlayerScreen(1);
			MapWidget->SetRenderTarget(MapRenderTarget);
		}
		else
		{
			UE_LOG(LogS1, Error, TEXT("AttackDefense: Could not create map widget."));
		}
	}
}

void AAttackDefensePlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!IsLocalPlayerController() || !IsValid(MapWidget))
	{
		return;
	}

	UpdateMapCapture();
	EnsureObjectivesCached();

	TArray<FMapMarkerData> Markers;
	GatherMapMarkers(Markers);
	MapWidget->UpdateMarkers(Markers);
}

void AAttackDefensePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (ToggleMapAction)
		{
			EIC->BindAction(ToggleMapAction, ETriggerEvent::Started, this, &AAttackDefensePlayerController::ToggleMap);
		}
	}
}

void AAttackDefensePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CaptureComponent)
	{
		CaptureComponent->DestroyComponent();
		CaptureComponent = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// Map capture
// ---------------------------------------------------------------------------

void AAttackDefensePlayerController::InitializeMapCapture()
{
	MapRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("MapRenderTarget"));
	MapRenderTarget->InitAutoFormat(MapResolution, MapResolution);
	MapRenderTarget->UpdateResourceImmediate(true);

	CaptureComponent = NewObject<USceneCaptureComponent2D>(this, TEXT("MapCapture"));
	CaptureComponent->RegisterComponent();
	CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureComponent->OrthoWidth = MinimapOrthoWidth;
	CaptureComponent->TextureTarget = MapRenderTarget;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComponent->bCaptureEveryFrame = true;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->SetWorldRotation(FRotator(-90.0f, 0.0f, 0.0f));
}

void AAttackDefensePlayerController::UpdateMapCapture()
{
	if (!CaptureComponent)
	{
		return;
	}

	if (bIsFullMap)
	{
		CaptureComponent->OrthoWidth = FullMapOrthoWidth;
		CaptureComponent->SetWorldLocation(FVector(FullMapCenter.X, FullMapCenter.Y, CaptureHeight));
	}
	else if (const APawn* MyPawn = GetPawn())
	{
		const FVector Loc = MyPawn->GetActorLocation();
		CaptureComponent->OrthoWidth = MinimapOrthoWidth;
		CaptureComponent->SetWorldLocation(FVector(Loc.X, Loc.Y, CaptureHeight));
	}
}

// ---------------------------------------------------------------------------
// Map markers
// ---------------------------------------------------------------------------

void AAttackDefensePlayerController::GatherMapMarkers(TArray<FMapMarkerData>& OutMarkers) const
{
	const AAttackDefenseGameState* GS = GetWorld()->GetGameState<AAttackDefenseGameState>();
	const AAttackDefensePlayerState* MyPS = GetPlayerState<AAttackDefensePlayerState>();

	// --- Self ---
	if (const APawn* MyPawn = GetPawn())
	{
		FMapMarkerData Self;
		Self.Type = EMapMarkerType::Self;
		Self.Position = WorldToMapPosition(MyPawn->GetActorLocation());
		Self.Rotation = MyPawn->GetActorRotation().Yaw;
		Self.Color = FLinearColor(0.0f, 0.5f, 1.0f);
		OutMarkers.Add(Self);
	}

	// --- Teammates ---
	if (GS && MyPS)
	{
		for (APlayerState* PS : GS->PlayerArray)
		{
			const AAttackDefensePlayerState* APS = Cast<AAttackDefensePlayerState>(PS);
			if (!APS || APS == MyPS || APS->TeamSide != MyPS->TeamSide || APS->bIsDead)
			{
				continue;
			}

			if (const APawn* TeammatePawn = APS->GetPawn())
			{
				FMapMarkerData Teammate;
				Teammate.Type = EMapMarkerType::Teammate;
				Teammate.Position = WorldToMapPosition(TeammatePawn->GetActorLocation());
				Teammate.Rotation = TeammatePawn->GetActorRotation().Yaw;
				Teammate.Color = FLinearColor(0.0f, 0.7f, 1.0f);
				OutMarkers.Add(Teammate);
			}
		}
	}

	// --- Objectives ---
	for (const TObjectPtr<AObjectiveActor>& Obj : CachedObjectives)
	{
		if (!IsValid(Obj))
		{
			continue;
		}

		FMapMarkerData Marker;
		Marker.Type = EMapMarkerType::Objective;
		Marker.Position = WorldToMapPosition(Obj->GetActorLocation());
		Marker.Progress = Obj->GetCaptureProgress();

		const TCHAR Letter = TEXT('A') + static_cast<TCHAR>(Obj->GetObjectiveIndex());
		Marker.Label = FText::FromString(FString(1, &Letter));

		const EObjectiveState ObjState = Obj->GetObjectiveState();
		Marker.bIsActive = (ObjState == EObjectiveState::Active || ObjState == EObjectiveState::Contested);

		switch (ObjState)
		{
		case EObjectiveState::Locked:    Marker.Color = FLinearColor(0.5f, 0.5f, 0.5f); break;
		case EObjectiveState::Active:    Marker.Color = FLinearColor(1.0f, 0.85f, 0.0f); break;
		case EObjectiveState::Contested: Marker.Color = FLinearColor(1.0f, 0.5f, 0.0f); break;
		case EObjectiveState::Captured:  Marker.Color = FLinearColor(0.0f, 1.0f, 0.0f); break;
		}

		OutMarkers.Add(Marker);
	}
}

FVector2D AAttackDefensePlayerController::WorldToMapPosition(const FVector& WorldPos) const
{
	if (!CaptureComponent)
	{
		return FVector2D(0.5, 0.5);
	}

	const FVector CapLoc = CaptureComponent->GetComponentLocation();
	const float OrthoW = CaptureComponent->OrthoWidth;

	// Top-down orthographic camera at FRotator(-90, 0, 0):
	//   Image U (right)  = World +Y
	//   Image V (down)   = World -X
	const double U = (WorldPos.Y - CapLoc.Y) / OrthoW + 0.5;
	const double V = -(WorldPos.X - CapLoc.X) / OrthoW + 0.5;

	return FVector2D(U, V);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AAttackDefensePlayerController::ToggleMap()
{
	bIsFullMap = !bIsFullMap;

	if (IsValid(MapWidget))
	{
		MapWidget->SetMapMode(bIsFullMap);
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void AAttackDefensePlayerController::EnsureObjectivesCached()
{
	if (CachedObjectives.Num() > 0)
	{
		return;
	}

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AObjectiveActor::StaticClass(), Found);

	for (AActor* Actor : Found)
	{
		if (AObjectiveActor* Obj = Cast<AObjectiveActor>(Actor))
		{
			CachedObjectives.Add(Obj);
		}
	}

	CachedObjectives.Sort([](const AObjectiveActor& A, const AObjectiveActor& B)
	{
		return A.GetObjectiveIndex() < B.GetObjectiveIndex();
	});
}
