#include "EditLayerApplicator.h"
#include "EditLayerManager.h"
#include "LevelToolSubsystem.h"
#include "DesignerIntentSubsystem.h"

#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BillboardComponent.h"
#include "ProceduralMeshComponent.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "Dom/JsonObject.h"

DEFINE_LOG_CATEGORY_STATIC(LogLayerApply, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::Initialize(UEditLayerManager* InLayerManager)
{
	LayerManager = InLayerManager;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply — 레이어 생성 또는 visible=true 전환
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyLayer(const FString& LayerId)
{
	if (!LayerManager) return;
	FEditLayer* Layer = LayerManager->FindLayer(LayerId);
	if (!Layer) return;

	FScopedTransaction Tx(FText::FromString(
		FString::Printf(TEXT("Apply Layer: %s"), *Layer->Label)));

	switch (Layer->Type)
	{
	case EEditLayerType::TerrainModify:    ApplyTerrainModify(*Layer);    break;
	case EEditLayerType::BuildingAdd:      ApplyBuildingAdd(*Layer);      break;
	case EEditLayerType::BuildingRemove:   ApplyBuildingRemove(*Layer);   break;
	case EEditLayerType::BuildingHeight:   ApplyBuildingHeight(*Layer);   break;
	case EEditLayerType::DestructionState: ApplyDestructionState(*Layer); break;
	case EEditLayerType::RoadBlock:        ApplyRoadBlock(*Layer);        break;
	case EEditLayerType::RoadAdd:          ApplyRoadAdd(*Layer);          break;
	case EEditLayerType::RoadWidth:        ApplyRoadWidth(*Layer);        break;
	case EEditLayerType::Marker:           ApplyMarker(*Layer);           break;
	case EEditLayerType::BuildingAddBatch: ApplyBuildingAddBatch(*Layer); break;
	default:
		UE_LOG(LogLayerApply, Warning, TEXT("Apply 미지원 타입: %d"), (int32)Layer->Type);
		break;
	}

	Layer->bVisible = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hide — visible=true → false 토글
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::HideLayer(const FString& LayerId)
{
	if (!LayerManager) return;
	FEditLayer* Layer = LayerManager->FindLayer(LayerId);
	if (!Layer) return;

	FScopedTransaction Tx(FText::FromString(
		FString::Printf(TEXT("Hide Layer: %s"), *Layer->Label)));

	switch (Layer->Type)
	{
	case EEditLayerType::TerrainModify:
		Layer->bVisible = false;
		FullReplayTerrainLayers();
		return;
	case EEditLayerType::BuildingAdd:
		HideBuildingAdd(*Layer);
		break;
	case EEditLayerType::BuildingRemove:
		HideBuildingRemove(*Layer);
		break;
	case EEditLayerType::BuildingHeight:
		HideBuildingHeight(*Layer);
		break;
	case EEditLayerType::DestructionState:
	case EEditLayerType::RoadBlock:
	case EEditLayerType::RoadAdd:
	case EEditLayerType::RoadWidth:
	case EEditLayerType::Marker:
		HideSpawnedActors(LayerId);
		break;
	default:
		break;
	}

	Layer->bVisible = false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Delete — 레이어 영구 제거
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::DeleteLayer(const FString& LayerId)
{
	if (!LayerManager) return;
	FEditLayer* Layer = LayerManager->FindLayer(LayerId);
	if (!Layer) return;

	FScopedTransaction Tx(FText::FromString(
		FString::Printf(TEXT("Delete Layer: %s"), *Layer->Label)));

	const bool bIsTerrain = (Layer->Type == EEditLayerType::TerrainModify);

	switch (Layer->Type)
	{
	case EEditLayerType::TerrainModify:
		break;
	case EEditLayerType::BuildingAdd:
	case EEditLayerType::RoadBlock:
	case EEditLayerType::RoadAdd:
	case EEditLayerType::RoadWidth:
	case EEditLayerType::Marker:
		DeleteSpawnedActors(LayerId);
		break;
	case EEditLayerType::BuildingRemove:
		RestoreOriginalActor(*Layer);
		break;
	case EEditLayerType::BuildingHeight:
		HideBuildingHeight(*Layer);
		break;
	case EEditLayerType::DestructionState:
		RestoreOriginalActor(*Layer);
		DeleteSpawnedActors(LayerId);
		break;
	default:
		break;
	}

	LayerManager->UnmapActorsForLayer(LayerId);
	LayerManager->RemoveLayer(LayerId);

	if (bIsTerrain)
	{
		FullReplayTerrainLayers();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — terrain_modify (Full Replay 방식)
// ─────────────────────────────────────────────────────────────────────────────

ALandscapeProxy* UEditLayerApplicator::FindLandscape() const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return nullptr;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		return *It;
	return nullptr;
}

void UEditLayerApplicator::CaptureTerrainBase(ALandscapeProxy* Landscape, ULandscapeInfo* Info)
{
	if (bTerrainBaseCaptured) return;

	FIntRect Bounds = Landscape->GetBoundingRect();
	TerrainBaseMinX = Bounds.Min.X;
	TerrainBaseMinY = Bounds.Min.Y;
	TerrainBaseMaxX = Bounds.Max.X;
	TerrainBaseMaxY = Bounds.Max.Y;

	const int32 W = TerrainBaseMaxX - TerrainBaseMinX + 1;
	const int32 H = TerrainBaseMaxY - TerrainBaseMinY + 1;
	TerrainBaseHeightmap.SetNumUninitialized(W * H);

	FLandscapeEditDataInterface Edit(Info);
	Edit.GetHeightDataFast(TerrainBaseMinX, TerrainBaseMinY,
		TerrainBaseMaxX, TerrainBaseMaxY, TerrainBaseHeightmap.GetData(), 0);

	bTerrainBaseCaptured = true;
	UE_LOG(LogLayerApply, Log, TEXT("terrain base snapshot: %dx%d"), W, H);

	SaveTerrainBaseToDisk();
}

void UEditLayerApplicator::RestoreTerrainBase(ULandscapeInfo* Info)
{
	if (!bTerrainBaseCaptured || TerrainBaseHeightmap.Num() == 0) return;

	FLandscapeEditDataInterface Edit(Info);
	Edit.SetHeightData(TerrainBaseMinX, TerrainBaseMinY,
		TerrainBaseMaxX, TerrainBaseMaxY, TerrainBaseHeightmap.GetData(), 0, true);
}

void UEditLayerApplicator::SaveTerrainBaseToDisk()
{
	if (TerrainBaseHeightmap.Num() == 0) return;

	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	if (!DIS) return;

	FString Dir = FPaths::GetPath(DIS->GetMapMetaPath());
	FString FilePath = Dir / TEXT("heightmap_base.bin");

	TArray<uint8> Header;
	Header.SetNumUninitialized(16);
	FMemory::Memcpy(&Header[0],  &TerrainBaseMinX, 4);
	FMemory::Memcpy(&Header[4],  &TerrainBaseMinY, 4);
	FMemory::Memcpy(&Header[8],  &TerrainBaseMaxX, 4);
	FMemory::Memcpy(&Header[12], &TerrainBaseMaxY, 4);

	TArray<uint8> Data;
	Data.Append(Header);
	Data.Append(reinterpret_cast<const uint8*>(TerrainBaseHeightmap.GetData()),
		TerrainBaseHeightmap.Num() * sizeof(uint16));

	FFileHelper::SaveArrayToFile(Data, *FilePath);
	UE_LOG(LogLayerApply, Log, TEXT("heightmap_base.bin 저장: %s (%d bytes)"), *FilePath, Data.Num());
}

bool UEditLayerApplicator::LoadTerrainBaseFromDisk()
{
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	if (!DIS) return false;

	FString Dir = FPaths::GetPath(DIS->GetMapMetaPath());
	FString FilePath = Dir / TEXT("heightmap_base.bin");

	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *FilePath)) return false;
	if (Data.Num() < 16) return false;

	FMemory::Memcpy(&TerrainBaseMinX, &Data[0],  4);
	FMemory::Memcpy(&TerrainBaseMinY, &Data[4],  4);
	FMemory::Memcpy(&TerrainBaseMaxX, &Data[8],  4);
	FMemory::Memcpy(&TerrainBaseMaxY, &Data[12], 4);

	const int32 ExpectedSize = (TerrainBaseMaxX - TerrainBaseMinX + 1) *
		(TerrainBaseMaxY - TerrainBaseMinY + 1);
	const int32 DataPixels = (Data.Num() - 16) / sizeof(uint16);
	if (DataPixels != ExpectedSize) return false;

	TerrainBaseHeightmap.SetNumUninitialized(ExpectedSize);
	FMemory::Memcpy(TerrainBaseHeightmap.GetData(), &Data[16], ExpectedSize * sizeof(uint16));

	bTerrainBaseCaptured = true;
	UE_LOG(LogLayerApply, Log, TEXT("heightmap_base.bin 로드: %dx%d"),
		TerrainBaseMaxX - TerrainBaseMinX + 1, TerrainBaseMaxY - TerrainBaseMinY + 1);
	return true;
}

void UEditLayerApplicator::ApplyTerrainModifySingle(
	const FEditLayer& Layer, ALandscapeProxy* Landscape, ULandscapeInfo* Info)
{
	if (!Layer.Params.IsValid()) return;

	const FString Operation = Layer.Params->GetStringField(TEXT("operation"));
	const float Strength    = Layer.Params->GetNumberField(TEXT("strength"));
	const FString Falloff   = Layer.Params->GetStringField(TEXT("falloff"));

	const FVector2D Center  = Layer.Area.Circle.CenterUE5;
	const float RadiusCm    = Layer.Area.Circle.RadiusCm;

	const FTransform LT       = Landscape->GetTransform();
	const FVector LScale      = LT.GetScale3D();
	const FVector LOrigin     = LT.GetLocation();
	const float QuadScaleX    = FMath::Max(1.f, FMath::Abs(LScale.X));
	const float QuadScaleY    = FMath::Max(1.f, FMath::Abs(LScale.Y));
	const float QuadScaleZ    = FMath::Max(1.f, FMath::Abs(LScale.Z));

	const float CenterLX = (Center.X - LOrigin.X) / QuadScaleX;
	const float CenterLY = (Center.Y - LOrigin.Y) / QuadScaleY;
	const float RadQ      = RadiusCm / QuadScaleX;

	int32 X1 = FMath::Max(TerrainBaseMinX, FMath::FloorToInt(CenterLX - RadQ));
	int32 Y1 = FMath::Max(TerrainBaseMinY, FMath::FloorToInt(CenterLY - RadQ));
	int32 X2 = FMath::Min(TerrainBaseMaxX, FMath::CeilToInt(CenterLX + RadQ));
	int32 Y2 = FMath::Min(TerrainBaseMaxY, FMath::CeilToInt(CenterLY + RadQ));

	const int32 W = X2 - X1 + 1;
	const int32 H = Y2 - Y1 + 1;
	if (W <= 0 || H <= 0) return;

	TArray<uint16> Hmap;
	Hmap.SetNumUninitialized(W * H);

	FLandscapeEditDataInterface Edit(Info);
	Edit.GetHeightDataFast(X1, Y1, X2, Y2, Hmap.GetData(), 0);

	const float HeightPerUnit = QuadScaleZ / 128.f;

	if (Operation == TEXT("offset"))
	{
		const float DeltaCm = Layer.Params->GetNumberField(TEXT("height_delta_cm"));
		const float DeltaUnits = DeltaCm / FMath::Max(0.001f, HeightPerUnit);

		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const float Dist = FMath::Sqrt(
					FMath::Square((float)(X1 + x) - CenterLX) +
					FMath::Square((float)(Y1 + y) - CenterLY));
				if (Dist > RadQ) continue;

				float Alpha = Strength;
				if (Falloff == TEXT("smooth"))
				{
					const float T = Dist / RadQ;
					Alpha *= 0.5f * (1.f + FMath::Cos(PI * T));
				}

				int32 Idx = y * W + x;
				int32 NewVal = (int32)Hmap[Idx] + FMath::RoundToInt(DeltaUnits * Alpha);
				Hmap[Idx] = (uint16)FMath::Clamp(NewVal, 0, 65535);
			}
		}
	}
	else if (Operation == TEXT("flatten"))
	{
		const float TargetCm = Layer.Params->GetNumberField(TEXT("target_height_cm"));
		const uint16 TargetUnit = (uint16)FMath::Clamp(
			FMath::RoundToInt(TargetCm / FMath::Max(0.001f, HeightPerUnit)) + 32768, 0, 65535);

		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const float Dist = FMath::Sqrt(
					FMath::Square((float)(X1 + x) - CenterLX) +
					FMath::Square((float)(Y1 + y) - CenterLY));
				if (Dist > RadQ) continue;

				float Alpha = Strength;
				if (Falloff == TEXT("smooth"))
				{
					const float T = Dist / RadQ;
					Alpha *= 0.5f * (1.f + FMath::Cos(PI * T));
				}

				int32 Idx = y * W + x;
				Hmap[Idx] = (uint16)FMath::RoundToInt(
					FMath::Lerp((float)Hmap[Idx], (float)TargetUnit, Alpha));
			}
		}
	}

	Edit.SetHeightData(X1, Y1, X2, Y2, Hmap.GetData(), 0, true);
}

void UEditLayerApplicator::FullReplayTerrainLayers()
{
	ALandscapeProxy* Landscape = FindLandscape();
	if (!Landscape || !bTerrainBaseCaptured) return;

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info) return;

	RestoreTerrainBase(Info);

	for (const FEditLayer& L : LayerManager->GetAllLayers())
	{
		if (L.Type == EEditLayerType::TerrainModify && L.bVisible)
		{
			ApplyTerrainModifySingle(L, Landscape, Info);
		}
	}
}

void UEditLayerApplicator::ApplyTerrainModify(FEditLayer& Layer)
{
	ALandscapeProxy* Landscape = FindLandscape();
	if (!Landscape) { UE_LOG(LogLayerApply, Warning, TEXT("terrain_modify: Landscape 없음")); return; }

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info) { UE_LOG(LogLayerApply, Warning, TEXT("terrain_modify: LandscapeInfo 없음")); return; }

	CaptureTerrainBase(Landscape, Info);
	ApplyTerrainModifySingle(Layer, Landscape, Info);

	SyncActorZInArea(Layer.Area.Circle.CenterUE5, Layer.Area.Circle.RadiusCm, Landscape);

	const FString Op = Layer.Params.IsValid() ? Layer.Params->GetStringField(TEXT("operation")) : TEXT("?");
	UE_LOG(LogLayerApply, Log, TEXT("terrain_modify: %s (%.0f, %.0f) R=%.0fcm"),
		*Op, Layer.Area.Circle.CenterUE5.X, Layer.Area.Circle.CenterUE5.Y,
		Layer.Area.Circle.RadiusCm);
}

// ─────────────────────────────────────────────────────────────────────────────
//  G-8: 지형 수정 후 영향 영역 내 건물/도로 Z 동기화
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::SyncActorZInArea(const FVector2D& Center, float RadiusCm, ALandscapeProxy* Landscape)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !Landscape) return;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A->Tags.Contains(FName(TEXT("LevelTool_Generated")))) continue;

		FVector Loc = A->GetActorLocation();
		FVector2D Loc2D(Loc.X, Loc.Y);
		if (FVector2D::Distance(Loc2D, Center) > RadiusCm) continue;

		float NewZ = GetTerrainZ(Loc.X, Loc.Y);
		if (FMath::Abs(NewZ - Loc.Z) > 1.f)
		{
			A->SetActorLocation(FVector(Loc.X, Loc.Y, NewZ));
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — building_add (P0-3 형상 분석 + P0-4 충돌 해소 포함)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyBuildingAdd(FEditLayer& Layer)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !Layer.Params.IsValid()) return;

	FString TypeKey     = Layer.Params->GetStringField(TEXT("type_key"));
	float   HeightM     = Layer.Params->GetNumberField(TEXT("height_m"));
	float   Rotation    = Layer.Params->GetNumberField(TEXT("rotation_deg"));
	float   FootprintM2 = Layer.Params->GetNumberField(TEXT("footprint_m2"));
	float   Aspect      = FMath::Max(1.f, (float)Layer.Params->GetNumberField(TEXT("aspect_ratio")));

	// P0-3: 인접 건물 형상 분석 — TypeKey/footprint/aspect 자동 도출
	if (TypeKey.IsEmpty() || TypeKey == TEXT("auto"))
	{
		auto* LTS = GEditor->GetEditorSubsystem<ULevelToolSubsystem>();
		auto* DIS = GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>();
		if (DIS && DIS->GetCachedBuildings().Num() > 0)
		{
			const FVector2D Loc2D(Layer.Area.Point.LocationUE5.X, Layer.Area.Point.LocationUE5.Y);
			const float SearchRadius = 30000.f; // 300m

			TMap<FString, int32> TypeCounts;
			TArray<float> NearFootprints;
			TArray<float> NearAspects;

			for (const auto& B : DIS->GetCachedBuildings())
			{
				if (FVector2D::Distance(B.CentroidUE5, Loc2D) < SearchRadius)
				{
					TypeCounts.FindOrAdd(B.TypeKey)++;
					NearFootprints.Add(B.AreaM2);
					if (B.AreaM2 > 0.f && B.HeightM > 0.f)
					{
						float Side = FMath::Sqrt(B.AreaM2);
						NearAspects.Add(FMath::Max(1.f, B.HeightM * 3.f / Side));
					}
				}
			}

			if (TypeCounts.Num() > 0)
			{
				FString BestType;
				int32 BestCount = 0;
				for (const auto& KV : TypeCounts)
				{
					if (KV.Value > BestCount) { BestCount = KV.Value; BestType = KV.Key; }
				}
				TypeKey = BestType;
				Layer.Params->SetStringField(TEXT("type_key"), TypeKey);
			}

			if (NearFootprints.Num() > 0)
			{
				NearFootprints.Sort();
				FootprintM2 = NearFootprints[NearFootprints.Num() / 2];
				Layer.Params->SetNumberField(TEXT("footprint_m2"), FootprintM2);
			}

			if (NearAspects.Num() > 0)
			{
				NearAspects.Sort();
				Aspect = NearAspects[NearAspects.Num() / 2];
				Layer.Params->SetNumberField(TEXT("aspect_ratio"), Aspect);
			}
		}
	}

	// P0-4: 배치 충돌 해소
	const float FootprintCm = FMath::Sqrt(FootprintM2) * 100.f;
	FVector DesiredLoc = Layer.Area.Point.LocationUE5;
	FVector FinalLoc = FindValidPlacement(DesiredLoc, FootprintCm, Layer.LayerId);

	const float Z = GetTerrainZ(FinalLoc.X, FinalLoc.Y);

	const float WidthM = FMath::Sqrt(FootprintM2 / Aspect);
	const float DepthM = WidthM * Aspect;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
		FVector(FinalLoc.X, FinalLoc.Y, Z),
		FRotator(0, Rotation, 0),
		Params);

	if (!Actor) return;

	UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
	if (Mesh)
	{
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (CubeMesh) Mesh->SetStaticMesh(CubeMesh);

		const float ScaleX = WidthM;
		const float ScaleY = DepthM;
		const float ScaleZ = HeightM;
		Actor->SetActorScale3D(FVector(ScaleX, ScaleY, ScaleZ));
	}

	const FString StableId = GenerateStableId(Layer);
	TagActor(Actor, StableId);
	Actor->SetActorLabel(FString::Printf(TEXT("Bldg_%s"), *StableId));
	Actor->SetFolderPath(TEXT("LevelTool/Buildings_Added"));

	LayerManager->MapActorToLayer(Layer.LayerId, Actor);

	UE_LOG(LogLayerApply, Log, TEXT("building_add: %s [%s] at (%.0f, %.0f, %.0f) fp=%.0fm² asp=%.1f"),
		*StableId, *TypeKey, FinalLoc.X, FinalLoc.Y, Z, FootprintM2, Aspect);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — building_remove
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyBuildingRemove(FEditLayer& Layer)
{
	FResolveResult Res = ResolveStableId(
		Layer.Area.ActorRef.StableId, Layer.Area.ActorRef.FallbackLocationUE5);

	AActor* Target = Res.Actor.Get();
	if (!Target)
	{
		UE_LOG(LogLayerApply, Warning,
			TEXT("building_remove: stable_id '%s' 해석 실패 (fallback %.0f,%.0f,%.0f)"),
			*Layer.Area.ActorRef.StableId,
			Layer.Area.ActorRef.FallbackLocationUE5.X,
			Layer.Area.ActorRef.FallbackLocationUE5.Y,
			Layer.Area.ActorRef.FallbackLocationUE5.Z);
		return;
	}

	Target->SetActorHiddenInGame(true);
	Target->SetActorEnableCollision(false);
#if WITH_EDITOR
	Target->SetIsTemporarilyHiddenInEditor(true);
#endif

	LayerManager->MapActorToLayer(Layer.LayerId, Target);

	UE_LOG(LogLayerApply, Log, TEXT("building_remove: %s 숨김 (Actor: %s)"),
		*Layer.Area.ActorRef.StableId, *Target->GetActorLabel());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — building_height
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyBuildingHeight(FEditLayer& Layer)
{
	FResolveResult Res = ResolveStableId(
		Layer.Area.ActorRef.StableId, Layer.Area.ActorRef.FallbackLocationUE5);

	AActor* Target = Res.Actor.Get();
	if (!Target || !Layer.Params.IsValid()) return;

	const float ScaleFactor = Layer.Params->GetNumberField(TEXT("scale_factor"));
	FVector Scale = Target->GetActorScale3D();
	Scale.Z *= ScaleFactor;
	Target->SetActorScale3D(Scale);

	LayerManager->MapActorToLayer(Layer.LayerId, Target);

	UE_LOG(LogLayerApply, Log, TEXT("building_height: %s scale_z=%.2f"),
		*Layer.Area.ActorRef.StableId, Scale.Z);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — destruction_state
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyDestructionState(FEditLayer& Layer)
{
	FResolveResult Res = ResolveStableId(
		Layer.Area.ActorRef.StableId, Layer.Area.ActorRef.FallbackLocationUE5);

	AActor* Target = Res.Actor.Get();
	if (!Target || !Layer.Params.IsValid()) return;

	const FString NewState = Layer.Params->GetStringField(TEXT("new_state"));

	if (NewState == TEXT("partial"))
	{
		FVector Scale = Target->GetActorScale3D();
		Scale.Z *= 0.85f;
		Target->SetActorScale3D(Scale);

		UStaticMeshComponent* SMC = Target->FindComponentByClass<UStaticMeshComponent>();
		if (SMC)
		{
			FString CurrentPath = SMC->GetStaticMesh() ? SMC->GetStaticMesh()->GetPathName() : TEXT("");
			FString DamagedPath = CurrentPath.Replace(TEXT("."), TEXT("_Damaged."), ESearchCase::CaseSensitive);
			UStaticMesh* DamagedMesh = LoadObject<UStaticMesh>(nullptr, *DamagedPath);
			if (DamagedMesh)
			{
				SMC->SetStaticMesh(DamagedMesh);
			}
		}
	}
	else if (NewState == TEXT("destroyed"))
	{
		UStaticMeshComponent* SMC = Target->FindComponentByClass<UStaticMeshComponent>();
		if (SMC)
		{
			FString CurrentPath = SMC->GetStaticMesh() ? SMC->GetStaticMesh()->GetPathName() : TEXT("");
			FString RuinPath = CurrentPath.Replace(TEXT("."), TEXT("_Ruin."), ESearchCase::CaseSensitive);
			UStaticMesh* RuinMesh = LoadObject<UStaticMesh>(nullptr, *RuinPath);
			if (RuinMesh)
			{
				SMC->SetStaticMesh(RuinMesh);
				Target->SetActorScale3D(FVector(1.f, 1.f, 0.3f));
			}
			else
			{
				Target->SetActorHiddenInGame(true);
				Target->SetActorEnableCollision(false);
			}
		}
		else
		{
			Target->SetActorHiddenInGame(true);
			Target->SetActorEnableCollision(false);
		}
	}

	LayerManager->MapActorToLayer(Layer.LayerId, Target);

	UE_LOG(LogLayerApply, Log, TEXT("destruction_state: %s → %s"),
		*Layer.Area.ActorRef.StableId, *NewState);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — road_block
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyRoadBlock(FEditLayer& Layer)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const FVector Location = Layer.Area.Point.LocationUE5;
	const float Z = GetTerrainZ(Location.X, Location.Y);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
		FVector(Location.X, Location.Y, Z), FRotator::ZeroRotator, Params);

	if (!Actor) return;

	UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
	if (Mesh)
	{
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (CubeMesh) Mesh->SetStaticMesh(CubeMesh);
		Actor->SetActorScale3D(FVector(3.f, 0.5f, 1.f));
	}

	const FString StableId = FString::Printf(TEXT("roadblock_%s"),
		*Layer.LayerId.Left(8));
	TagActor(Actor, StableId);
	Actor->SetActorLabel(FString::Printf(TEXT("RoadBlock_%s"), *Layer.LayerId.Left(8)));
	Actor->SetFolderPath(TEXT("LevelTool/RoadBlocks"));

	LayerManager->MapActorToLayer(Layer.LayerId, Actor);

	UE_LOG(LogLayerApply, Log, TEXT("road_block: %s"), *StableId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — road_add (ProceduralMesh 도로 생성)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyRoadAdd(FEditLayer& Layer)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const TArray<FVector2D>& PathPts = Layer.Area.Path.PointsUE5;
	if (PathPts.Num() < 2) return;

	const float HalfW = Layer.Area.Path.WidthCm * 0.5f;

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	Vertices.Reserve(PathPts.Num() * 2);
	Normals.Reserve(PathPts.Num() * 2);
	UVs.Reserve(PathPts.Num() * 2);
	Triangles.Reserve((PathPts.Num() - 1) * 6);

	float AccLen = 0.f;
	for (int32 i = 0; i < PathPts.Num(); ++i)
	{
		FVector2D Dir;
		if (i == 0)
			Dir = (PathPts[1] - PathPts[0]).GetSafeNormal();
		else if (i == PathPts.Num() - 1)
			Dir = (PathPts[i] - PathPts[i - 1]).GetSafeNormal();
		else
			Dir = ((PathPts[i + 1] - PathPts[i]).GetSafeNormal()
				+ (PathPts[i] - PathPts[i - 1]).GetSafeNormal()).GetSafeNormal();

		const FVector2D Perp(-Dir.Y, Dir.X);
		const float Z = GetTerrainZ(PathPts[i].X, PathPts[i].Y) + 5.f;

		Vertices.Add(FVector(PathPts[i].X + Perp.X * HalfW, PathPts[i].Y + Perp.Y * HalfW, Z));
		Vertices.Add(FVector(PathPts[i].X - Perp.X * HalfW, PathPts[i].Y - Perp.Y * HalfW, Z));
		Normals.Add(FVector::UpVector);
		Normals.Add(FVector::UpVector);

		if (i > 0) AccLen += FVector2D::Distance(PathPts[i], PathPts[i - 1]);
		const float U = AccLen / FMath::Max(1.f, Layer.Area.Path.WidthCm);
		UVs.Add(FVector2D(U, 0.f));
		UVs.Add(FVector2D(U, 1.f));
	}

	for (int32 i = 0; i < PathPts.Num() - 1; ++i)
	{
		const int32 B = i * 2;
		Triangles.Append({ B, B + 2, B + 1,  B + 1, B + 2, B + 3 });
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* RoadActor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!RoadActor) return;

	UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(RoadActor);
	RoadActor->SetRootComponent(ProcMesh);
	ProcMesh->RegisterComponent();
	ProcMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, true);
	ProcMesh->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));

	const FString StableId = GenerateStableId(Layer);
	TagActor(RoadActor, StableId);
	RoadActor->SetActorLabel(FString::Printf(TEXT("Road_%s"), *StableId));
	RoadActor->SetFolderPath(TEXT("LevelTool/Roads_Added"));

	LayerManager->MapActorToLayer(Layer.LayerId, RoadActor);

	UE_LOG(LogLayerApply, Log, TEXT("road_add: %s (%d pts, w=%.0fcm)"),
		*StableId, PathPts.Num(), Layer.Area.Path.WidthCm);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — road_width (기존 도로 폭 변경 — ProceduralMesh 재빌드)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyRoadWidth(FEditLayer& Layer)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const FString TargetStableId = Layer.Area.ActorRef.StableId;
	float NewWidthCm = 600.f;
	if (Layer.Params.IsValid() && Layer.Params->HasField(TEXT("width_m")))
		NewWidthCm = Layer.Params->GetNumberField(TEXT("width_m")) * 100.f;

	AActor* TargetRoad = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(*TargetStableId))
		{
			TargetRoad = *It;
			break;
		}
	}

	if (!TargetRoad)
	{
		UE_LOG(LogLayerApply, Warning, TEXT("road_width: 대상 도로 '%s' 없음"), *TargetStableId);
		return;
	}

	UProceduralMeshComponent* ProcMesh = TargetRoad->FindComponentByClass<UProceduralMeshComponent>();
	if (!ProcMesh)
	{
		UE_LOG(LogLayerApply, Warning, TEXT("road_width: ProceduralMeshComponent 없음"));
		return;
	}

	// Find the source RoadAdd layer to get path points
	auto* DIS = GEditor ? GEditor->GetEditorSubsystem<UDesignerIntentSubsystem>() : nullptr;
	auto* LM = DIS ? DIS->GetLayerManager() : nullptr;
	if (!LM) return;

	const TArray<FVector2D>* PathPts = nullptr;
	for (const FEditLayer& L : LM->GetAllLayers())
	{
		if (L.Type == EEditLayerType::RoadAdd)
		{
			FString GenId = FString::Printf(TEXT("road_add_%s"), *L.LayerId.Left(8));
			if (GenId == TargetStableId || TargetRoad->Tags.Contains(*GenId))
			{
				PathPts = &L.Area.Path.PointsUE5;
				break;
			}
		}
	}

	if (!PathPts || PathPts->Num() < 2) return;

	const float HalfW = NewWidthCm * 0.5f;

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	Vertices.Reserve(PathPts->Num() * 2);
	float AccLen = 0.f;
	for (int32 i = 0; i < PathPts->Num(); ++i)
	{
		FVector2D Dir;
		if (i == 0)
			Dir = ((*PathPts)[1] - (*PathPts)[0]).GetSafeNormal();
		else if (i == PathPts->Num() - 1)
			Dir = ((*PathPts)[i] - (*PathPts)[i - 1]).GetSafeNormal();
		else
			Dir = (((*PathPts)[i + 1] - (*PathPts)[i]).GetSafeNormal()
				+ ((*PathPts)[i] - (*PathPts)[i - 1]).GetSafeNormal()).GetSafeNormal();

		const FVector2D Perp(-Dir.Y, Dir.X);
		const float Z = GetTerrainZ((*PathPts)[i].X, (*PathPts)[i].Y) + 5.f;

		Vertices.Add(FVector((*PathPts)[i].X + Perp.X * HalfW, (*PathPts)[i].Y + Perp.Y * HalfW, Z));
		Vertices.Add(FVector((*PathPts)[i].X - Perp.X * HalfW, (*PathPts)[i].Y - Perp.Y * HalfW, Z));
		Normals.Add(FVector::UpVector);
		Normals.Add(FVector::UpVector);

		if (i > 0) AccLen += FVector2D::Distance((*PathPts)[i], (*PathPts)[i - 1]);
		const float U = AccLen / FMath::Max(1.f, NewWidthCm);
		UVs.Add(FVector2D(U, 0.f));
		UVs.Add(FVector2D(U, 1.f));
	}

	for (int32 i = 0; i < PathPts->Num() - 1; ++i)
	{
		const int32 B = i * 2;
		Triangles.Append({ B, B + 2, B + 1,  B + 1, B + 2, B + 3 });
	}

	ProcMesh->ClearAllMeshSections();
	ProcMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, true);

	UE_LOG(LogLayerApply, Log, TEXT("road_width: '%s' → %.0fcm"), *TargetStableId, NewWidthCm);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — marker (Billboard)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyMarker(FEditLayer& Layer)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const FVector Location = Layer.Area.Point.LocationUE5;
	const float Z = GetTerrainZ(Location.X, Location.Y) + 200.f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* MarkerActor = World->SpawnActor<AActor>(
		FVector(Location.X, Location.Y, Z), FRotator::ZeroRotator, Params);
	if (!MarkerActor) return;

	UBillboardComponent* Billboard = NewObject<UBillboardComponent>(MarkerActor);
	Billboard->SetupAttachment(MarkerActor->GetRootComponent());
	Billboard->RegisterComponent();
	Billboard->SetRelativeScale3D(FVector(2.f));

	MarkerActor->SetRootComponent(Billboard);

	const FString StableId = FString::Printf(TEXT("marker_%s"),
		*Layer.LayerId.Left(8));
	TagActor(MarkerActor, StableId);
	MarkerActor->SetActorLabel(FString::Printf(TEXT("Marker_%s"), *Layer.Label));
	MarkerActor->SetFolderPath(TEXT("LevelTool/Markers"));

#if WITH_EDITOR
	MarkerActor->SetIsTemporarilyHiddenInEditor(false);
#endif

	LayerManager->MapActorToLayer(Layer.LayerId, MarkerActor);

	UE_LOG(LogLayerApply, Log, TEXT("marker: %s at (%.0f, %.0f)"),
		*Layer.Label, Location.X, Location.Y);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hide 분기
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::HideBuildingAdd(FEditLayer& Layer)
{
	HideSpawnedActors(Layer.LayerId);
}

void UEditLayerApplicator::HideBuildingRemove(FEditLayer& Layer)
{
	auto Actors = LayerManager->GetMappedActors(Layer.LayerId);
	for (auto& Weak : Actors)
	{
		if (AActor* A = Weak.Get())
		{
			A->SetActorHiddenInGame(false);
			A->SetActorEnableCollision(true);
#if WITH_EDITOR
			A->SetIsTemporarilyHiddenInEditor(false);
#endif
		}
	}
}

void UEditLayerApplicator::HideBuildingHeight(FEditLayer& Layer)
{
	auto Actors = LayerManager->GetMappedActors(Layer.LayerId);
	for (auto& Weak : Actors)
	{
		if (AActor* A = Weak.Get())
		{
			FVector S = A->GetActorScale3D();
			const float Factor = Layer.Params.IsValid()
				? (float)Layer.Params->GetNumberField(TEXT("scale_factor")) : 1.f;
			if (Factor > 0.f) S.Z /= Factor;
			A->SetActorScale3D(S);
		}
	}
}

void UEditLayerApplicator::HideSpawnedActors(const FString& LayerId)
{
	auto Actors = LayerManager->GetMappedActors(LayerId);
	for (auto& Weak : Actors)
	{
		if (AActor* A = Weak.Get())
		{
			A->SetActorHiddenInGame(true);
			A->SetActorEnableCollision(false);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Delete 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::DeleteSpawnedActors(const FString& LayerId)
{
	auto Actors = LayerManager->GetMappedActors(LayerId);
	for (auto& Weak : Actors)
	{
		if (AActor* A = Weak.Get())
		{
			A->Destroy();
		}
	}
	LayerManager->UnmapActorsForLayer(LayerId);
}

void UEditLayerApplicator::RestoreOriginalActor(FEditLayer& Layer)
{
	auto Actors = LayerManager->GetMappedActors(Layer.LayerId);
	for (auto& Weak : Actors)
	{
		if (AActor* A = Weak.Get())
		{
			A->SetActorHiddenInGame(false);
			A->SetActorEnableCollision(true);
			A->SetActorScale3D(FVector::OneVector);
#if WITH_EDITOR
			A->SetIsTemporarilyHiddenInEditor(false);
#endif
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  P0-4: 배치 충돌 해소 — AABB 중복 / 수역 / 경사 / 4방향 재시도
// ─────────────────────────────────────────────────────────────────────────────

bool UEditLayerApplicator::CheckPlacementCollision(
	const FVector& Location, float FootprintCm, const FString& SkipLayerId) const
{
	if (!LayerManager) return false;
	const float HalfFP = FootprintCm * 0.5f;

	for (const FEditLayer& L : LayerManager->GetAllLayers())
	{
		if (L.LayerId == SkipLayerId) continue;
		if (L.Type != EEditLayerType::BuildingAdd || !L.bVisible) continue;

		const FVector Other = L.Area.Point.LocationUE5;
		const float OtherFP = L.Params.IsValid()
			? FMath::Sqrt((float)L.Params->GetNumberField(TEXT("footprint_m2"))) * 100.f
			: 1000.f;
		const float OtherHalf = OtherFP * 0.5f;

		if (FMath::Abs(Location.X - Other.X) < (HalfFP + OtherHalf) &&
		    FMath::Abs(Location.Y - Other.Y) < (HalfFP + OtherHalf))
		{
			return true;
		}
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World)
	{
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A->Tags.Contains(FName(TEXT("LevelTool_Generated")))) continue;

			const FVector ALoc = A->GetActorLocation();
			const FVector AExt = A->GetComponentsBoundingBox().GetExtent();
			if (FMath::Abs(Location.X - ALoc.X) < (HalfFP + AExt.X) &&
			    FMath::Abs(Location.Y - ALoc.Y) < (HalfFP + AExt.Y))
			{
				return true;
			}
		}
	}
	return false;
}

bool UEditLayerApplicator::CheckSlopeAt(float WorldX, float WorldY, float MaxSlopeDeg) const
{
	const float Step = 200.f;
	const float ZC = GetTerrainZ(WorldX, WorldY);
	float MaxDiff = 0.f;

	for (int32 d = 0; d < 4; ++d)
	{
		const float DX = (d < 2) ? ((d == 0) ? Step : -Step) : 0.f;
		const float DY = (d >= 2) ? ((d == 2) ? Step : -Step) : 0.f;
		const float ZN = GetTerrainZ(WorldX + DX, WorldY + DY);
		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(ZN - ZC));
	}

	const float SlopeDeg = FMath::RadiansToDegrees(FMath::Atan2(MaxDiff, Step));
	return SlopeDeg > MaxSlopeDeg;
}

bool UEditLayerApplicator::CheckWaterAt(float WorldX, float WorldY) const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return false;

	const float Z = GetTerrainZ(WorldX, WorldY);
	FVector Start(WorldX, WorldY, Z + 5000.f);
	FVector End(WorldX, WorldY, Z - 100.f);

	FHitResult Hit;
	FCollisionQueryParams QP;
	QP.bTraceComplex = false;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, QP))
	{
		if (Hit.GetActor() && Hit.GetActor()->GetName().Contains(TEXT("Water")))
			return true;
	}
	return false;
}

FVector UEditLayerApplicator::FindValidPlacement(
	const FVector& Desired, float FootprintCm, const FString& SkipLayerId) const
{
	if (!CheckPlacementCollision(Desired, FootprintCm, SkipLayerId) &&
	    !CheckSlopeAt(Desired.X, Desired.Y) &&
	    !CheckWaterAt(Desired.X, Desired.Y))
	{
		return Desired;
	}

	const float Offsets[] = { 1500.f, 3000.f, 5000.f, 8000.f };
	const FVector2D Dirs[] = {
		FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1)
	};

	for (float Off : Offsets)
	{
		for (const FVector2D& Dir : Dirs)
		{
			FVector Candidate(Desired.X + Dir.X * Off, Desired.Y + Dir.Y * Off, 0.f);
			if (!CheckPlacementCollision(Candidate, FootprintCm, SkipLayerId) &&
			    !CheckSlopeAt(Candidate.X, Candidate.Y) &&
			    !CheckWaterAt(Candidate.X, Candidate.Y))
			{
				UE_LOG(LogLayerApply, Log,
					TEXT("placement retry: (%.0f,%.0f)→(%.0f,%.0f) offset=%.0f"),
					Desired.X, Desired.Y, Candidate.X, Candidate.Y, Off);
				return Candidate;
			}
		}
	}

	UE_LOG(LogLayerApply, Warning, TEXT("placement: 유효 위치 미발견 — 원위치 사용"));
	return Desired;
}

// ─────────────────────────────────────────────────────────────────────────────
//  stable_id 해석
// ─────────────────────────────────────────────────────────────────────────────

FResolveResult UEditLayerApplicator::ResolveStableId(
	const FString& StableId, const FVector& FallbackLocation) const
{
	FResolveResult Result;

	// 1. Tag 직접 검색
	AActor* Found = FindActorByStableId(StableId);
	if (Found)
	{
		Result.Actor  = Found;
		Result.Status = EResolveStatus::Direct;
		return Result;
	}

	// 2. Fallback: 반경 10000cm (100m) 내 가장 가까운 LevelTool 태그 Actor
	if (!FallbackLocation.IsNearlyZero())
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World)
		{
			float BestDist = 10000.f;
			AActor* BestActor = nullptr;

			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				if (!A->Tags.Contains(FName(TEXT("LevelTool_Generated")))) continue;

				const float Dist = FVector::Dist(A->GetActorLocation(), FallbackLocation);
				if (Dist < BestDist)
				{
					BestDist  = Dist;
					BestActor = A;
				}
			}

			if (BestActor)
			{
				Result.Actor  = BestActor;
				Result.Status = EResolveStatus::Fallback;
				UE_LOG(LogLayerApply, Log,
					TEXT("ResolveStableId fallback: '%s' → Actor '%s' (%.0fcm)"),
					*StableId, *BestActor->GetName(), BestDist);
				return Result;
			}
		}
	}

	// 3. 실패
	Result.Status = EResolveStatus::Missing;
	UE_LOG(LogLayerApply, Warning, TEXT("ResolveStableId: '%s' 해석 실패"), *StableId);
	return Result;
}

AActor* UEditLayerApplicator::FindActorByStableId(const FString& StableId) const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return nullptr;

	const FName IdTag(*StableId);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A->Tags.Contains(FName(TEXT("LevelTool_StableID"))) &&
			A->Tags.Contains(IdTag))
		{
			return A;
		}
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  유틸리티
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::TagActor(AActor* Actor, const FString& StableId) const
{
	if (!Actor) return;
	Actor->Tags.AddUnique(FName(TEXT("LevelTool_StableID")));
	Actor->Tags.AddUnique(FName(*StableId));
	Actor->Tags.AddUnique(FName(TEXT("LevelTool_Generated")));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Apply 분기 — building_add_batch (배치 레이어 일괄 적용)
// ─────────────────────────────────────────────────────────────────────────────

void UEditLayerApplicator::ApplyBuildingAddBatch(FEditLayer& Layer)
{
	if (!Layer.Params.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Layer.Params->TryGetArrayField(TEXT("batch_items"), Items)) return;

	for (int32 i = 0; i < Items->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Item = (*Items)[i]->AsObject();
		if (!Item) continue;

		FEditLayer SubLayer;
		SubLayer.LayerId = FString::Printf(TEXT("%s_%d"), *Layer.LayerId, i);
		SubLayer.Type = EEditLayerType::BuildingAdd;
		SubLayer.CreatedBy = Layer.CreatedBy;
		SubLayer.Label = FString::Printf(TEXT("%s [%d/%d]"), *Layer.Label, i + 1, Items->Num());

		const TArray<TSharedPtr<FJsonValue>>& Loc = Item->GetArrayField(TEXT("location_ue5"));
		if (Loc.Num() >= 3)
		{
			SubLayer.Area.Type = EAreaType::Point;
			SubLayer.Area.Point.LocationUE5 = FVector(
				Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
		}

		SubLayer.Params = MakeShared<FJsonObject>();
		SubLayer.Params->SetStringField(TEXT("type_key"), Item->GetStringField(TEXT("type_key")));
		SubLayer.Params->SetNumberField(TEXT("height_m"), Item->GetNumberField(TEXT("height_m")));
		SubLayer.Params->SetNumberField(TEXT("rotation_deg"), Item->GetNumberField(TEXT("rotation_deg")));
		if (Item->HasField(TEXT("footprint_m2")))
			SubLayer.Params->SetNumberField(TEXT("footprint_m2"), Item->GetNumberField(TEXT("footprint_m2")));

		ApplyBuildingAdd(SubLayer);
	}

	UE_LOG(LogLayerApply, Log, TEXT("building_add_batch: %s (%d items)"),
		*Layer.Label, Items->Num());
}

FString UEditLayerApplicator::GenerateStableId(const FEditLayer& Layer)
{
	const FString Prefix =
		(Layer.Type == EEditLayerType::BuildingAdd || Layer.Type == EEditLayerType::BuildingAddBatch)
			? TEXT("bldg_add_")
		: (Layer.Type == EEditLayerType::RoadAdd)
			? TEXT("road_add_")
			: TEXT("layer_");

	return Prefix + Layer.LayerId.Left(8);
}

float UEditLayerApplicator::GetTerrainZ(float WorldX, float WorldY) const
{
	auto* LTS = GEditor ? GEditor->GetEditorSubsystem<ULevelToolSubsystem>() : nullptr;
	if (!LTS) return 0.f;

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return 0.f;

	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		FVector Start(WorldX, WorldY, 100000.f);
		FVector End(WorldX, WorldY, -100000.f);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic))
		{
			return Hit.Location.Z;
		}
		break;
	}
	return 0.f;
}
