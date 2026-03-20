#include "HeatmapGenerator.h"
#include "DesignerIntentSubsystem.h"
#include "ChecklistEngine.h"
#include "ChecklistTypes.h"
#include "LevelToolPerfGuard.h"
#include "Engine/Texture2D.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeProxy.h"

DEFINE_LOG_CATEGORY_STATIC(LogHeatmap, Log, All);

const FHeatmapData UHeatmapGenerator::EmptyHeatmap = {};

static const float kCellSizeCm = 5000.f;  // 50m

// ─────────────────────────────────────────────────────────────────────────────
void UHeatmapGenerator::Initialize(UDesignerIntentSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

int32 UHeatmapGenerator::GridHalf() const
{
	if (!Subsystem || Subsystem->MapRadiusKm <= 0.f) return 10;
	return FMath::CeilToInt(Subsystem->MapRadiusKm * 100000.f / kCellSizeCm);
}

float UHeatmapGenerator::CellSize() const { return kCellSizeCm; }

// ─────────────────────────────────────────────────────────────────────────────
FHeatmapData UHeatmapGenerator::Generate(EHeatmapMode Mode) const
{
	switch (Mode)
	{
	case EHeatmapMode::BuildingDensity: return GenerateBuildingDensity();
	case EHeatmapMode::RoadConnectivity: return GenerateRoadConnectivity();
	case EHeatmapMode::PoiDistribution: return GeneratePoiDistribution();
	case EHeatmapMode::Elevation: return GenerateElevation();
	default: return {};
	}
}

const FHeatmapData& UHeatmapGenerator::GetCachedHeatmap(EHeatmapMode Mode) const
{
	const FHeatmapData* Found = CachedMaps.Find(Mode);
	return Found ? *Found : EmptyHeatmap;
}

void UHeatmapGenerator::GenerateAll()
{
	LEVELTOOL_SCOPED_TIMER(HeatmapGenerate);
	CachedMaps.Add(EHeatmapMode::BuildingDensity, GenerateBuildingDensity());
	CachedMaps.Add(EHeatmapMode::RoadConnectivity, GenerateRoadConnectivity());
	CachedMaps.Add(EHeatmapMode::PoiDistribution, GeneratePoiDistribution());
	CachedMaps.Add(EHeatmapMode::Elevation, GenerateElevation());

	UE_LOG(LogHeatmap, Log, TEXT("히트맵 4종 생성 완료 (셀 크기 %.0fm, 반지름 %d셀)"),
		kCellSizeCm / 100.f, GridHalf());
}

// ─────────────────────────────────────────────────────────────────────────────
//  건물 밀도 히트맵
// ─────────────────────────────────────────────────────────────────────────────

FHeatmapData UHeatmapGenerator::GenerateBuildingDensity() const
{
	FHeatmapData Data;
	Data.Mode = EHeatmapMode::BuildingDensity;
	Data.CellSizeCm = kCellSizeCm;
	Data.Label = TEXT("건물 밀도");
	if (!Subsystem) return Data;

	const int32 Half = GridHalf();
	Data.GridResolution = Half * 2 + 1;
	const float RadCm = Subsystem->MapRadiusKm * 100000.f;

	TMap<int64, int32> CellCounts;
	auto Key = [Half](int32 gx, int32 gy) -> int64 { return (int64)(gx + Half) * 10000 + (gy + Half); };

	for (const auto& B : Subsystem->CachedBuildings)
	{
		int32 gx = FMath::FloorToInt(B.CentroidUE5.X / kCellSizeCm);
		int32 gy = FMath::FloorToInt(B.CentroidUE5.Y / kCellSizeCm);
		gx = FMath::Clamp(gx, -Half, Half);
		gy = FMath::Clamp(gy, -Half, Half);
		CellCounts.FindOrAdd(Key(gx, gy))++;
	}

	float MaxCount = 1.f;
	for (const auto& KV : CellCounts) MaxCount = FMath::Max(MaxCount, (float)KV.Value);

	for (int32 gx = -Half; gx <= Half; ++gx)
	{
		for (int32 gy = -Half; gy <= Half; ++gy)
		{
			FVector2D Center(gx * kCellSizeCm, gy * kCellSizeCm);
			if (Center.Size() > RadCm) continue;

			int32* Count = CellCounts.Find(Key(gx, gy));
			float V = Count ? (float)(*Count) / MaxCount : 0.f;

			FHeatmapCell C;
			C.GridX = gx;
			C.GridY = gy;
			C.WorldCenter = Center;
			C.Value = V;
			C.Color = ValueToColor_RedGreen(V);
			Data.Cells.Add(MoveTemp(C));
		}
	}

	Data.MinValue = 0.f;
	Data.MaxValue = MaxCount;
	return Data;
}

// ─────────────────────────────────────────────────────────────────────────────
//  도로 연결성 히트맵
// ─────────────────────────────────────────────────────────────────────────────

FHeatmapData UHeatmapGenerator::GenerateRoadConnectivity() const
{
	FHeatmapData Data;
	Data.Mode = EHeatmapMode::RoadConnectivity;
	Data.CellSizeCm = kCellSizeCm;
	Data.Label = TEXT("도로 연결성");
	if (!Subsystem) return Data;

	const int32 Half = GridHalf();
	Data.GridResolution = Half * 2 + 1;
	const float RadCm = Subsystem->MapRadiusKm * 100000.f;

	TMap<int64, int32> CellRoadCount;
	auto Key = [Half](int32 gx, int32 gy) -> int64 { return (int64)(gx + Half) * 10000 + (gy + Half); };

	for (const auto& R : Subsystem->CachedRoads)
	{
		for (const FVector2D& Pt : R.PointsUE5)
		{
			int32 gx = FMath::Clamp(FMath::FloorToInt(Pt.X / kCellSizeCm), -Half, Half);
			int32 gy = FMath::Clamp(FMath::FloorToInt(Pt.Y / kCellSizeCm), -Half, Half);
			CellRoadCount.FindOrAdd(Key(gx, gy))++;
		}
	}

	float MaxR = 1.f;
	for (const auto& KV : CellRoadCount) MaxR = FMath::Max(MaxR, (float)KV.Value);

	for (int32 gx = -Half; gx <= Half; ++gx)
	{
		for (int32 gy = -Half; gy <= Half; ++gy)
		{
			FVector2D Center(gx * kCellSizeCm, gy * kCellSizeCm);
			if (Center.Size() > RadCm) continue;

			int32* Count = CellRoadCount.Find(Key(gx, gy));
			float V = Count ? FMath::Min(1.f, (float)(*Count) / MaxR) : 0.f;

			FHeatmapCell C;
			C.GridX = gx;
			C.GridY = gy;
			C.WorldCenter = Center;
			C.Value = V;
			C.Color = ValueToColor_RedGreen(V);
			Data.Cells.Add(MoveTemp(C));
		}
	}

	Data.MinValue = 0.f;
	Data.MaxValue = MaxR;
	return Data;
}

// ─────────────────────────────────────────────────────────────────────────────
//  거점 분포 히트맵
// ─────────────────────────────────────────────────────────────────────────────

FHeatmapData UHeatmapGenerator::GeneratePoiDistribution() const
{
	FHeatmapData Data;
	Data.Mode = EHeatmapMode::PoiDistribution;
	Data.CellSizeCm = kCellSizeCm;
	Data.Label = TEXT("거점 분포");
	if (!Subsystem || !Subsystem->GetChecklistEngine()) return Data;

	const int32 Half = GridHalf();
	Data.GridResolution = Half * 2 + 1;
	const float RadCm = Subsystem->MapRadiusKm * 100000.f;

	const FCheckReport& Report = Subsystem->GetChecklistEngine()->GetLastReport();
	const float PoiRadiusCm = 15000.f; // 150m DBSCAN eps

	for (int32 gx = -Half; gx <= Half; ++gx)
	{
		for (int32 gy = -Half; gy <= Half; ++gy)
		{
			FVector2D Center(gx * kCellSizeCm, gy * kCellSizeCm);
			if (Center.Size() > RadCm) continue;

			float V = 0.f;
			for (const FPoiCluster& P : Report.DetectedPois)
			{
				float Dist = FVector2D::Distance(Center, P.Centroid);
				if (Dist < PoiRadiusCm)
				{
					float Contribution = 1.f - (Dist / PoiRadiusCm);
					if (P.Grade == TEXT("S"))      Contribution *= 1.0f;
					else if (P.Grade == TEXT("A")) Contribution *= 0.7f;
					else if (P.Grade == TEXT("B")) Contribution *= 0.4f;
					else                            Contribution *= 0.2f;
					V = FMath::Max(V, Contribution);
				}
			}

			FHeatmapCell C;
			C.GridX = gx;
			C.GridY = gy;
			C.WorldCenter = Center;
			C.Value = V;
			C.Color = ValueToColor_RedGreen(V);
			Data.Cells.Add(MoveTemp(C));
		}
	}

	Data.MinValue = 0.f;
	Data.MaxValue = 1.f;
	return Data;
}

// ─────────────────────────────────────────────────────────────────────────────
//  고저차 히트맵
// ─────────────────────────────────────────────────────────────────────────────

FHeatmapData UHeatmapGenerator::GenerateElevation() const
{
	FHeatmapData Data;
	Data.Mode = EHeatmapMode::Elevation;
	Data.CellSizeCm = kCellSizeCm;
	Data.Label = TEXT("고저차");
	if (!Subsystem) return Data;

	const int32 Half = GridHalf();
	Data.GridResolution = Half * 2 + 1;
	const float RadCm = Subsystem->MapRadiusKm * 100000.f;

	ALandscapeProxy* Landscape = nullptr;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			Landscape = *It;
			break;
		}
	}

	float ActualMin = FLT_MAX, ActualMax = -FLT_MAX;
	TArray<TPair<FHeatmapCell, float>> PendingCells;

	for (int32 gx = -Half; gx <= Half; ++gx)
	{
		for (int32 gy = -Half; gy <= Half; ++gy)
		{
			FVector2D Center(gx * kCellSizeCm, gy * kCellSizeCm);
			if (Center.Size() > RadCm) continue;

			float HeightCm = 0.f;
			bool bSampled = false;

			if (Landscape)
			{
				FVector WorldLoc(Center.X, Center.Y, 50000.f);
				TOptional<float> H = Landscape->GetHeightAtLocation(WorldLoc);
				if (H.IsSet())
				{
					HeightCm = H.GetValue();
					bSampled = true;
				}
			}

			if (!bSampled)
			{
				float DistRatio = Center.Size() / FMath::Max(1.f, RadCm);
				float ElevRange = FMath::Max(1.f, Subsystem->ElevationMaxM - Subsystem->ElevationMinM);
				HeightCm = (Subsystem->ElevationMinM + ElevRange * (1.f - DistRatio) * 0.5f) * 100.f;
			}

			ActualMin = FMath::Min(ActualMin, HeightCm);
			ActualMax = FMath::Max(ActualMax, HeightCm);

			FHeatmapCell C;
			C.GridX = gx;
			C.GridY = gy;
			C.WorldCenter = Center;
			PendingCells.Add(TPair<FHeatmapCell, float>{MoveTemp(C), HeightCm});
		}
	}

	const float Range = FMath::Max(1.f, ActualMax - ActualMin);
	Data.MinValue = ActualMin / 100.f;
	Data.MaxValue = ActualMax / 100.f;

	for (auto& Pair : PendingCells)
	{
		Pair.Key.Value = FMath::Clamp((Pair.Value - ActualMin) / Range, 0.f, 1.f);
		Pair.Key.Color = ValueToColor_Elevation(Pair.Key.Value);
		Data.Cells.Add(MoveTemp(Pair.Key));
	}

	return Data;
}

// ─────────────────────────────────────────────────────────────────────────────
//  색상 매핑
// ─────────────────────────────────────────────────────────────────────────────

FLinearColor UHeatmapGenerator::ValueToColor_RedGreen(float V) const
{
	V = FMath::Clamp(V, 0.f, 1.f);
	return FLinearColor(
		FMath::Lerp(0.1f, 1.0f, V),
		FMath::Lerp(0.8f, 0.2f, V),
		0.1f, 0.6f);
}

FLinearColor UHeatmapGenerator::ValueToColor_Elevation(float V) const
{
	V = FMath::Clamp(V, 0.f, 1.f);
	if (V < 0.25f) return FLinearColor(0.1f, 0.4f, 0.1f, 0.5f);
	if (V < 0.50f) return FLinearColor(0.6f, 0.7f, 0.2f, 0.5f);
	if (V < 0.75f) return FLinearColor(0.7f, 0.5f, 0.2f, 0.5f);
	return FLinearColor(0.9f, 0.3f, 0.2f, 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  텍스처 렌더링
// ─────────────────────────────────────────────────────────────────────────────

UTexture2D* UHeatmapGenerator::RenderToTexture(const FHeatmapData& Data) const
{
	if (Data.GridResolution <= 0) return nullptr;

	const int32 Res = Data.GridResolution;
	UTexture2D* Tex = UTexture2D::CreateTransient(Res, Res, PF_B8G8R8A8);
	if (!Tex) return nullptr;

	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void* TexData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memzero(TexData, Res * Res * 4);

	uint8* Pixels = static_cast<uint8*>(TexData);

	const int32 Half = Res / 2;
	for (const FHeatmapCell& C : Data.Cells)
	{
		int32 px = C.GridX + Half;
		int32 py = C.GridY + Half;
		if (px < 0 || px >= Res || py < 0 || py >= Res) continue;

		int32 Idx = (py * Res + px) * 4;
		Pixels[Idx + 0] = FMath::Clamp(FMath::RoundToInt(C.Color.B * 255.f), 0, 255);
		Pixels[Idx + 1] = FMath::Clamp(FMath::RoundToInt(C.Color.G * 255.f), 0, 255);
		Pixels[Idx + 2] = FMath::Clamp(FMath::RoundToInt(C.Color.R * 255.f), 0, 255);
		Pixels[Idx + 3] = FMath::Clamp(FMath::RoundToInt(C.Color.A * 255.f), 0, 255);
	}

	Mip.BulkData.Unlock();
	Tex->UpdateResource();
	return Tex;
}

// ─────────────────────────────────────────────────────────────────────────────
//  뷰포트 데칼 오버레이
// ─────────────────────────────────────────────────────────────────────────────

UMaterial* UHeatmapGenerator::GetOrCreateDecalMaterial()
{
	if (CachedDecalMaterial) return CachedDecalMaterial;

#if WITH_EDITOR
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_LevelTool_HeatmapDecal"), RF_Transient);
	Mat->MaterialDomain = MD_DeferredDecal;
	Mat->BlendMode = BLEND_Translucent;

	auto* TexParam = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
	TexParam->ParameterName = TEXT("HeatmapTexture");
	TexParam->SamplerType = SAMPLERTYPE_Color;

	auto* EditorData = Mat->GetEditorOnlyData();
	EditorData->ExpressionCollection.Expressions.Add(TexParam);
	EditorData->EmissiveColor.Connect(0, TexParam);
	EditorData->Opacity.Connect(4, TexParam);

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();

	CachedDecalMaterial = Mat;
#endif
	return CachedDecalMaterial;
}

void UHeatmapGenerator::ShowHeatmapOverlay(EHeatmapMode Mode)
{
	HideHeatmapOverlay();

	const FHeatmapData& Data = GetCachedHeatmap(Mode);
	if (Data.Cells.Num() == 0)
	{
		UE_LOG(LogHeatmap, Warning, TEXT("히트맵 데이터 없음 — GenerateAll 먼저 실행하세요"));
		return;
	}

	UTexture2D* Tex = RenderToTexture(Data);
	if (!Tex) return;
	ActiveOverlayTexture = Tex;

	UMaterial* DecalMat = GetOrCreateDecalMaterial();
	if (!DecalMat)
	{
		UE_LOG(LogHeatmap, Error, TEXT("히트맵 데칼 머티리얼 생성 실패"));
		return;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return;

	const float MapSize = Subsystem ? Subsystem->MapRadiusKm * 2.f * 100000.f : 200000.f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADecalActor* Decal = World->SpawnActor<ADecalActor>(
		FVector(0, 0, 50000.f), FRotator(-90, 0, 0), Params);

	if (!Decal) return;

	UDecalComponent* DC = Decal->GetDecal();
	if (DC)
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(DecalMat, this);
		if (MID)
		{
			MID->SetTextureParameterValue(TEXT("HeatmapTexture"), Tex);
		}

		DC->SetDecalMaterial(MID);
		DC->DecalSize = FVector(100000.f, MapSize * 0.5f, MapSize * 0.5f);
	}

	Decal->SetActorLabel(FString::Printf(TEXT("Heatmap_%s"), *Data.Label));
	Decal->SetFolderPath(TEXT("LevelTool/Heatmaps"));
	Decal->Tags.Add(FName(TEXT("LevelTool_Heatmap")));

#if WITH_EDITOR
	Decal->SetIsTemporarilyHiddenInEditor(false);
#endif

	ActiveDecal = Decal;
	ActiveOverlayMode = Mode;

	UE_LOG(LogHeatmap, Log, TEXT("히트맵 오버레이 표시: %s"), *Data.Label);
}

void UHeatmapGenerator::HideHeatmapOverlay()
{
	if (ADecalActor* D = ActiveDecal.Get())
	{
		D->Destroy();
	}
	ActiveDecal = nullptr;
	ActiveOverlayTexture = nullptr;
}
