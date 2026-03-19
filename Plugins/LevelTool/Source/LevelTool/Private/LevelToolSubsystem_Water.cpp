#include "LevelToolSubsystem.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "ScopedTransaction.h"

void ULevelToolSubsystem::SpawnWaterBodies(const FString& JsonPath, const FBox& LandscapeBounds)
{
    if (!FPaths::FileExists(JsonPath))
    {
        Log(FString::Printf(TEXT("  ℹ Water JSON not found: %s — skipping"), *JsonPath));
        return;
    }

    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *JsonPath)) return;

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        Log(TEXT("  ⚠ Failed to parse water JSON"));
        return;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    UMaterial* BaseMat = LoadObject<UMaterial>(nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "SpawnWater", "LevelTool: Spawn Water Bodies"));

    const FString Folder = TEXT("LevelTool/Water");
    const FLinearColor LakeColor(0.05f, 0.15f, 0.35f);
    const FLinearColor RiverColor(0.08f, 0.20f, 0.40f);

    int32 LakeCount  = 0;
    int32 RiverCount = 0;

    // ── Lakes: closed polygon → flat ProceduralMesh ─────────────────────
    const TArray<TSharedPtr<FJsonValue>>* LakesArr;
    if (Root->TryGetArrayField(TEXT("lakes"), LakesArr))
    {
        for (const TSharedPtr<FJsonValue>& LakeVal : *LakesArr)
        {
            const TSharedPtr<FJsonObject>& LakeObj = LakeVal->AsObject();
            if (!LakeObj) continue;

            const TArray<TSharedPtr<FJsonValue>>& PtsJson = LakeObj->GetArrayField(TEXT("points_ue5"));
            if (PtsJson.Num() < 3) continue;

            TArray<FVector> Pts;
            for (const TSharedPtr<FJsonValue>& PtVal : PtsJson)
            {
                const TArray<TSharedPtr<FJsonValue>>& Pt = PtVal->AsArray();
                if (Pt.Num() < 2) continue;
                float X = (float)Pt[0]->AsNumber();
                float Y = (float)Pt[1]->AsNumber();

                if (LandscapeBounds.IsValid &&
                    (X < LandscapeBounds.Min.X - 5000.f || X > LandscapeBounds.Max.X + 5000.f ||
                     Y < LandscapeBounds.Min.Y - 5000.f || Y > LandscapeBounds.Max.Y + 5000.f))
                    continue;

                float Z = GetTerrainZAtWorldXY(X, Y) - 30.f;
                Pts.Add(FVector(X, Y, Z));
            }
            if (Pts.Num() < 3) continue;

            // Find average Z for flat water surface
            float AvgZ = 0.f;
            for (const FVector& P : Pts) AvgZ += P.Z;
            AvgZ /= Pts.Num();

            // Fan triangulation from centroid
            FVector Centroid = FVector::ZeroVector;
            for (const FVector& P : Pts) Centroid += P;
            Centroid /= Pts.Num();
            Centroid.Z = AvgZ;

            TArray<FVector>   Vertices;
            TArray<int32>     Triangles;
            TArray<FVector>   Normals;
            TArray<FVector2D> UVs;

            Vertices.Add(Centroid);
            Normals.Add(FVector::UpVector);
            UVs.Add(FVector2D(0.5f, 0.5f));

            for (int32 i = 0; i < Pts.Num(); i++)
            {
                FVector V = Pts[i];
                V.Z = AvgZ;
                Vertices.Add(V);
                Normals.Add(FVector::UpVector);
                UVs.Add(FVector2D(
                    (V.X - Centroid.X) / 10000.f + 0.5f,
                    (V.Y - Centroid.Y) / 10000.f + 0.5f));
            }

            for (int32 i = 1; i <= Pts.Num(); i++)
            {
                int32 Next = (i % Pts.Num()) + 1;
                Triangles.Add(0);
                Triangles.Add(i);
                Triangles.Add(Next);
            }

            FActorSpawnParameters Params;
            Params.bNoFail = true;
            AActor* LakeActor = World->SpawnActor<AActor>(
                AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
            if (!LakeActor) continue;

            UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(LakeActor, TEXT("WaterMesh"));
            PMC->SetupAttachment(nullptr);
            PMC->RegisterComponent();
            LakeActor->SetRootComponent(PMC);
            LakeActor->AddInstanceComponent(PMC);
            PMC->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

            TArray<FLinearColor> Colors;
            TArray<FProcMeshTangent> Tangents;
            PMC->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);

            if (BaseMat)
            {
                UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, LakeActor);
                Mat->SetVectorParameterValue(TEXT("Color"),
                    FVector4(LakeColor.R, LakeColor.G, LakeColor.B, 1.f));
                PMC->SetMaterial(0, Mat);
            }

            int64 LakeId = (int64)LakeObj->GetNumberField(TEXT("id"));
            LakeActor->SetActorLabel(FString::Printf(TEXT("Lake_%lld"), LakeId));
            LakeActor->SetFolderPath(*Folder);
            LakeCount++;
        }
    }

    // ── Rivers: polyline → strip ProceduralMesh ─────────────────────────
    const TArray<TSharedPtr<FJsonValue>>* RiversArr;
    if (Root->TryGetArrayField(TEXT("rivers"), RiversArr))
    {
        for (const TSharedPtr<FJsonValue>& RiverVal : *RiversArr)
        {
            const TSharedPtr<FJsonObject>& RiverObj = RiverVal->AsObject();
            if (!RiverObj) continue;

            float WidthM = (float)RiverObj->GetNumberField(TEXT("width_m"));
            float HalfWidthCm = FMath::Max(WidthM * 50.f, 200.f);

            const TArray<TSharedPtr<FJsonValue>>& PtsJson = RiverObj->GetArrayField(TEXT("points_ue5"));
            if (PtsJson.Num() < 2) continue;

            TArray<FVector> Pts;
            for (const TSharedPtr<FJsonValue>& PtVal : PtsJson)
            {
                const TArray<TSharedPtr<FJsonValue>>& Pt = PtVal->AsArray();
                if (Pt.Num() < 2) continue;
                float X = (float)Pt[0]->AsNumber();
                float Y = (float)Pt[1]->AsNumber();

                if (LandscapeBounds.IsValid &&
                    (X < LandscapeBounds.Min.X - 5000.f || X > LandscapeBounds.Max.X + 5000.f ||
                     Y < LandscapeBounds.Min.Y - 5000.f || Y > LandscapeBounds.Max.Y + 5000.f))
                    continue;

                float Z = GetTerrainZAtWorldXY(X, Y) - 20.f;
                Pts.Add(FVector(X, Y, Z));
            }
            if (Pts.Num() < 2) continue;

            TArray<FVector>   Vertices;
            TArray<int32>     Triangles;
            TArray<FVector>   RNormals;
            TArray<FVector2D> UVs;
            float UOffset = 0.f;

            for (int32 i = 0; i < Pts.Num() - 1; i++)
            {
                const FVector& P0 = Pts[i];
                const FVector& P1 = Pts[i + 1];
                FVector Dir   = (P1 - P0).GetSafeNormal2D();
                FVector Right = FVector(-Dir.Y, Dir.X, 0.f);

                FVector V0 = P0 - Right * HalfWidthCm;
                FVector V1 = P0 + Right * HalfWidthCm;
                FVector V2 = P1 + Right * HalfWidthCm;
                FVector V3 = P1 - Right * HalfWidthCm;

                float SegLen = FVector::Dist(P0, P1) / 100.f;
                int32 Base = Vertices.Num();
                Vertices.Append({ V0, V1, V2, V3 });
                RNormals.Append({ FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector });
                UVs.Append({
                    { UOffset,          0.f },
                    { UOffset,          1.f },
                    { UOffset + SegLen, 1.f },
                    { UOffset + SegLen, 0.f }
                });
                UOffset += SegLen;
                Triangles.Append({ Base, Base+1, Base+2, Base, Base+2, Base+3 });
            }

            FActorSpawnParameters Params;
            Params.bNoFail = true;
            AActor* RiverActor = World->SpawnActor<AActor>(
                AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
            if (!RiverActor) continue;

            UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(RiverActor, TEXT("RiverMesh"));
            PMC->SetupAttachment(nullptr);
            PMC->RegisterComponent();
            RiverActor->SetRootComponent(PMC);
            RiverActor->AddInstanceComponent(PMC);
            PMC->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

            TArray<FLinearColor> Colors;
            TArray<FProcMeshTangent> Tangents;
            PMC->CreateMeshSection_LinearColor(0, Vertices, Triangles, RNormals, UVs, Colors, Tangents, false);

            if (BaseMat)
            {
                UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, RiverActor);
                Mat->SetVectorParameterValue(TEXT("Color"),
                    FVector4(RiverColor.R, RiverColor.G, RiverColor.B, 1.f));
                PMC->SetMaterial(0, Mat);
            }

            int64 RiverId = (int64)RiverObj->GetNumberField(TEXT("id"));
            RiverActor->SetActorLabel(FString::Printf(TEXT("River_%lld"), RiverId));
            RiverActor->SetFolderPath(*Folder);
            RiverCount++;
        }
    }

    if (LakeCount > 0 || RiverCount > 0)
    {
        Log(FString::Printf(TEXT("  ✔ Water bodies: %d lakes, %d rivers → LevelTool/Water"), LakeCount, RiverCount));
    }
    else
    {
        Log(TEXT("  ℹ No water bodies to spawn"));
    }
}
