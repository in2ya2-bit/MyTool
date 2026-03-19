#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"

#include "Misc/FileHelper.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "EngineUtils.h"
#include "Landscape.h"

void ULevelToolSubsystem::SpawnRoadActors(const FString& JsonPath, const FBox& LandscapeBounds)
{
    if (!FPaths::FileExists(JsonPath))
    {
        Log(FString::Printf(TEXT("  ⚠ Roads JSON not found: %s"), *JsonPath));
        return;
    }

    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return;

    TArray<TSharedPtr<FJsonValue>> JsonArray;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, JsonArray)) return;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    UMaterial* BaseMat = LoadObject<UMaterial>(nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    struct FRoadProps { float HalfWidthCm; FLinearColor Color; float ZPriorityCm; };
    auto GetProps = [](const FString& Type) -> FRoadProps
    {
        if (Type == TEXT("major")) return { 600.f, FLinearColor(0.08f, 0.08f, 0.08f), 6.f };
        if (Type == TEXT("path"))  return { 100.f, FLinearColor(0.55f, 0.50f, 0.38f), 0.f };
        return                            { 300.f, FLinearColor(0.18f, 0.18f, 0.18f), 3.f };
    };

    struct FRoadGeom
    {
        TArray<FVector>     Vertices;
        TArray<int32>       Triangles;
        TArray<FVector>     Normals;
        TArray<FVector2D>   UVs;
        float               UOffset = 0.f;
    };
    TMap<FString, FRoadGeom> Geoms;
    Geoms.Add(TEXT("major"), {});
    Geoms.Add(TEXT("minor"), {});
    Geoms.Add(TEXT("path"),  {});

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "SpawnRoads", "LevelTool: Spawn Roads"));

    int32 TotalSegs = 0;
    int32 Skipped   = 0;

    for (const TSharedPtr<FJsonValue>& Val : JsonArray)
    {
        const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
        if (!Obj) continue;

        FString RoadType = Obj->GetStringField(TEXT("type"));
        float RoadWidthM = (float)Obj->GetNumberField(TEXT("width_m"));
        const TArray<TSharedPtr<FJsonValue>>& PtsJson = Obj->GetArrayField(TEXT("points_ue5"));
        if (PtsJson.Num() < 2) { Skipped++; continue; }

        TArray<FVector> Pts;
        for (const TSharedPtr<FJsonValue>& PtVal : PtsJson)
        {
            const TArray<TSharedPtr<FJsonValue>>& Pt = PtVal->AsArray();
            if (Pt.Num() < 2) continue;
            float X = (float)Pt[0]->AsNumber();
            float Y = (float)Pt[1]->AsNumber();

            if (LandscapeBounds.IsValid &&
                (X < LandscapeBounds.Min.X || X > LandscapeBounds.Max.X ||
                 Y < LandscapeBounds.Min.Y || Y > LandscapeBounds.Max.Y))
                continue;

            const float Z = GetTerrainZAtWorldXY(X, Y) + 3.f;
            Pts.Add(FVector(X, Y, Z));
        }
        if (Pts.Num() < 2) { Skipped++; continue; }

        {
            constexpr float MaxSegLenCm = 500.f;
            TArray<FVector> Sub;
            Sub.Reserve(Pts.Num() * 4);
            Sub.Add(Pts[0]);
            for (int32 j = 1; j < Pts.Num(); j++)
            {
                float Dist2D = FVector::Dist2D(Pts[j-1], Pts[j]);
                int32 NumSubs = FMath::Max(1, FMath::CeilToInt32(Dist2D / MaxSegLenCm));
                for (int32 s = 1; s <= NumSubs; s++)
                {
                    float A  = static_cast<float>(s) / static_cast<float>(NumSubs);
                    float IX = FMath::Lerp(Pts[j-1].X, Pts[j].X, A);
                    float IY = FMath::Lerp(Pts[j-1].Y, Pts[j].Y, A);
                    float IZ = GetTerrainZAtWorldXY(IX, IY) + 3.f;
                    Sub.Add(FVector(IX, IY, IZ));
                }
            }
            Pts = MoveTemp(Sub);
        }

        {
            TArray<float> SmoothedZ;
            SmoothedZ.SetNumUninitialized(Pts.Num());
            for (int32 j = 0; j < Pts.Num(); j++)
            {
                float Sum   = 0.f;
                int32 Count = 0;
                for (int32 k = FMath::Max(0, j - 1);
                     k <= FMath::Min(Pts.Num() - 1, j + 1); k++)
                {
                    Sum += Pts[k].Z;
                    Count++;
                }
                SmoothedZ[j] = Sum / Count;
            }
            for (int32 j = 0; j < Pts.Num(); j++)
                Pts[j].Z = SmoothedZ[j];
        }

        if (!Geoms.Contains(RoadType)) RoadType = TEXT("minor");
        FRoadGeom&  G     = Geoms[RoadType];
        FRoadProps  Props = GetProps(RoadType);

        if (RoadWidthM > 0.f)
            Props.HalfWidthCm = RoadWidthM * 50.f;

        const float ZOff = 3.f + Props.ZPriorityCm;
        for (int32 i = 0; i < Pts.Num() - 1; i++)
        {
            const FVector& P0  = Pts[i];
            const FVector& P1  = Pts[i + 1];
            FVector Dir   = (P1 - P0).GetSafeNormal2D();
            FVector Right = FVector(-Dir.Y, Dir.X, 0.f);

            FVector V0 = P0 - Right * Props.HalfWidthCm;
            FVector V1 = P0 + Right * Props.HalfWidthCm;
            FVector V2 = P1 + Right * Props.HalfWidthCm;
            FVector V3 = P1 - Right * Props.HalfWidthCm;

            V0.Z = GetTerrainZAtWorldXY(V0.X, V0.Y) + ZOff;
            V1.Z = GetTerrainZAtWorldXY(V1.X, V1.Y) + ZOff;
            V2.Z = GetTerrainZAtWorldXY(V2.X, V2.Y) + ZOff;
            V3.Z = GetTerrainZAtWorldXY(V3.X, V3.Y) + ZOff;

            FVector FaceN = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
            if (FaceN.Z < 0.f) FaceN = -FaceN;

            float SegM = FVector::Dist(P0, P1) / 100.f;
            int32 Base = G.Vertices.Num();
            G.Vertices.Append({ V0, V1, V2, V3 });
            G.Normals.Append({ FaceN, FaceN, FaceN, FaceN });
            G.UVs.Append({
                { G.UOffset,        0.f },
                { G.UOffset,        1.f },
                { G.UOffset + SegM, 1.f },
                { G.UOffset + SegM, 0.f }
            });
            G.UOffset += SegM;
            G.Triangles.Append({ Base, Base+1, Base+2, Base, Base+2, Base+3 });
            TotalSegs++;
        }
    }

    int32 SpawnedActors = 0;
    for (auto& KV : Geoms)
    {
        FRoadGeom& G = KV.Value;
        if (G.Vertices.IsEmpty()) continue;

        FActorSpawnParameters Params;
        Params.bNoFail = true;
        AActor* RoadActor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
        if (!RoadActor) continue;

        UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(RoadActor, TEXT("RoadMesh"));
        PMC->SetupAttachment(nullptr);
        PMC->RegisterComponent();
        RoadActor->SetRootComponent(PMC);
        RoadActor->AddInstanceComponent(PMC);
        PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        TArray<FLinearColor>    Colors;
        TArray<FProcMeshTangent> Tangents;
        PMC->CreateMeshSection_LinearColor(0,
            G.Vertices, G.Triangles, G.Normals, G.UVs, Colors, Tangents, false);

        if (BaseMat)
        {
            FRoadProps Props = GetProps(KV.Key);
            UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, RoadActor);
            Mat->SetVectorParameterValue(TEXT("Color"),
                FVector4(Props.Color.R, Props.Color.G, Props.Color.B, 1.f));
            PMC->SetMaterial(0, Mat);
        }

        RoadActor->SetActorLabel(FString::Printf(TEXT("Roads_%s"), *KV.Key));
        RoadActor->Tags.Add(*KV.Key);
        RoadActor->SetFolderPath(TEXT("LevelTool/Roads"));
        SpawnedActors++;
    }

    Log(FString::Printf(TEXT("  ✔ Roads: %d segments → %d actors  (%d ways skipped)"),
        TotalSegs, SpawnedActors, Skipped));
}
