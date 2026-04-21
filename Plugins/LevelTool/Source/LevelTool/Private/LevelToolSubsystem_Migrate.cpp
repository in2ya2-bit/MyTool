// LevelToolSubsystem_Migrate.cpp
//
// Content Browser > Asset Actions > Migrate 를 깨끗하게 돌리기 위한 유틸리티.
//
//  1) BakeRoadsToStaticMesh  — UProceduralMeshComponent 로 만든 도로를
//                              실제 UStaticMesh 에셋으로 구워서 저장하고
//                              AStaticMeshActor 로 교체 (migrate 안전).
//  2) CleanupForMigrate      — Compass/TextRender 등 디버그 액터 정리.
//  3) PackAssetsUnderMapData — /Game/LevelTool/... 하위 에셋을
//                              /Game/MapData/<MapName>/... 로 이동.
//
// 이 파일은 기존 LevelToolSubsystem 의 퍼블릭 API 를 구현하는 용도로,
// 헤더는 LevelToolSubsystem.h 하나만 쓰고 있습니다.
#include "LevelToolSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/TextRenderActor.h"

#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace LevelToolMigrate
{
    // Sanitize a user-provided map name so it is safe as a long package path segment.
    static FString SanitizeMapName(const FString& InName)
    {
        FString Clean = InName.IsEmpty() ? FString(TEXT("GeneratedMap")) : InName;
        const TArray<TCHAR> Bad = { TEXT('/'), TEXT('\\'), TEXT(':'), TEXT('?'),
                                    TEXT('*'), TEXT('"'), TEXT('<'), TEXT('>'),
                                    TEXT('|'), TEXT(' '), TEXT('.') };
        for (TCHAR C : Bad) Clean.ReplaceCharInline(C, TEXT('_'));
        return Clean;
    }

    // Convert a single ProceduralMesh section into a StaticMesh asset.
    // Returns the created (and saved) UStaticMesh, or nullptr on failure.
    static UStaticMesh* BakeProcMeshSectionToStaticMesh(
        UProceduralMeshComponent* PMC,
        int32 SectionIndex,
        UMaterialInterface* Material,
        const FString& PackagePath,     // e.g. /Game/MapData/GenMap/Roads
        const FString& AssetName)       // e.g. SM_Roads_major
    {
        if (!PMC) return nullptr;

        const FProcMeshSection* Section = PMC->GetProcMeshSection(SectionIndex);
        if (!Section || Section->ProcVertexBuffer.Num() == 0 || Section->ProcIndexBuffer.Num() == 0)
        {
            return nullptr;
        }

        const FString FullPackage = PackagePath / AssetName;
        UPackage* Package = CreatePackage(*FullPackage);
        if (!Package) return nullptr;
        Package->FullyLoad();

        UStaticMesh* NewMesh = NewObject<UStaticMesh>(
            Package, *AssetName, RF_Public | RF_Standalone);
        NewMesh->InitResources();
        NewMesh->SetLightingGuid();

        // Build FMeshDescription from the PMC section
        FMeshDescription MeshDesc;
        FStaticMeshAttributes Attr(MeshDesc);
        Attr.Register();

        TVertexAttributesRef<FVector3f>      VertexPositions = Attr.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals   = Attr.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents  = Attr.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float>     VertexInstanceBinormalSigns = Attr.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors    = Attr.GetVertexInstanceColors();
        TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs       = Attr.GetVertexInstanceUVs();
        TPolygonGroupAttributesRef<FName>       PolygonGroupMaterialSlot = Attr.GetPolygonGroupMaterialSlotNames();

        VertexInstanceUVs.SetNumChannels(1);

        FPolygonGroupID PolygonGroupID = MeshDesc.CreatePolygonGroup();
        PolygonGroupMaterialSlot[PolygonGroupID] = FName(TEXT("Road"));

        const int32 NumVerts = Section->ProcVertexBuffer.Num();
        TArray<FVertexID> VertexIDs;
        VertexIDs.Reserve(NumVerts);
        for (int32 i = 0; i < NumVerts; ++i)
        {
            const FProcMeshVertex& Src = Section->ProcVertexBuffer[i];
            FVertexID VID = MeshDesc.CreateVertex();
            VertexPositions[VID] = FVector3f(Src.Position);
            VertexIDs.Add(VID);
        }

        const TArray<uint32>& Indices = Section->ProcIndexBuffer;
        for (int32 Tri = 0; Tri < Indices.Num(); Tri += 3)
        {
            const uint32 I0 = Indices[Tri + 0];
            const uint32 I1 = Indices[Tri + 1];
            const uint32 I2 = Indices[Tri + 2];
            if (I0 >= (uint32)NumVerts || I1 >= (uint32)NumVerts || I2 >= (uint32)NumVerts) continue;

            auto MakeInstance = [&](uint32 Idx) -> FVertexInstanceID
            {
                FVertexInstanceID VIID = MeshDesc.CreateVertexInstance(VertexIDs[Idx]);
                const FProcMeshVertex& V = Section->ProcVertexBuffer[Idx];
                VertexInstanceNormals [VIID] = FVector3f(V.Normal);
                VertexInstanceTangents[VIID] = FVector3f(V.Tangent.TangentX);
                VertexInstanceBinormalSigns[VIID] = V.Tangent.bFlipTangentY ? -1.f : 1.f;
                VertexInstanceColors  [VIID] = FVector4f(V.Color.R / 255.f, V.Color.G / 255.f,
                                                         V.Color.B / 255.f, V.Color.A / 255.f);
                VertexInstanceUVs.Set(VIID, 0, FVector2f(V.UV0));
                return VIID;
            };

            FVertexInstanceID VI0 = MakeInstance(I0);
            FVertexInstanceID VI1 = MakeInstance(I1);
            FVertexInstanceID VI2 = MakeInstance(I2);

            MeshDesc.CreatePolygon(PolygonGroupID, { VI0, VI1, VI2 });
        }

        // Per-instance normals/tangents are already written above from the PMC section.
        // We still need polygon-level normals so the build pipeline can stitch everything
        // correctly; the per-instance data is preserved.
        FStaticMeshOperations::ComputePolygonTangentsAndNormals(MeshDesc);

        // Material slot
        NewMesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(TEXT("Road"))));

        // Build the mesh
        TArray<const FMeshDescription*> Descs;
        Descs.Add(&MeshDesc);

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bBuildSimpleCollision = false;
        BuildParams.bFastBuild = true;
        BuildParams.bMarkPackageDirty = true;
        NewMesh->BuildFromMeshDescriptions(Descs, BuildParams);

        NewMesh->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(NewMesh);

        // Save to disk so migrate has something to copy
        const FString Filename = FPackageName::LongPackageNameToFilename(
            FullPackage, FPackageName::GetAssetPackageExtension());
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        UPackage::SavePackage(Package, NewMesh, *Filename, SaveArgs);

        return NewMesh;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BakeRoadsToStaticMesh
// ─────────────────────────────────────────────────────────────────────────────
int32 ULevelToolSubsystem::BakeRoadsToStaticMesh(const FString& MapName)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Log(TEXT("✖ BakeRoads: no editor world"));
        return 0;
    }

    const FString CleanName = LevelToolMigrate::SanitizeMapName(MapName);
    const FString RoadsPkgRoot = FString::Printf(TEXT("/Game/MapData/%s/Roads"), *CleanName);

    FScopedTransaction Transaction(
        NSLOCTEXT("LevelTool", "BakeRoads", "LevelTool: Bake Roads to StaticMesh"));

    // Collect every actor in LevelTool/Roads folder that owns a PMC
    TArray<AActor*> RoadActors;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) continue;

        const bool bInRoadsFolder = A->GetFolderPath().ToString().StartsWith(TEXT("LevelTool/Roads"));
        const bool bHasPMC = A->FindComponentByClass<UProceduralMeshComponent>() != nullptr;
        if (bInRoadsFolder && bHasPMC)
        {
            RoadActors.Add(A);
        }
    }

    if (RoadActors.IsEmpty())
    {
        Log(TEXT("ℹ BakeRoads: no ProceduralMeshComponent road actors found"));
        return 0;
    }

    Log(FString::Printf(TEXT("▶ BakeRoads: baking %d road actor(s) into /Game/MapData/%s/Roads/"),
        RoadActors.Num(), *CleanName));

    int32 BakedCount = 0;
    TArray<AActor*> ToDestroy;

    for (AActor* RoadActor : RoadActors)
    {
        UProceduralMeshComponent* PMC = RoadActor->FindComponentByClass<UProceduralMeshComponent>();
        if (!PMC) continue;

        const int32 NumSections = PMC->GetNumSections();
        if (NumSections <= 0) continue;

        // We only bake the primary (section 0) which is what the roads generator writes.
        UMaterialInterface* Mat = PMC->GetMaterial(0);

        FString Label = RoadActor->GetActorLabel();
        if (Label.IsEmpty()) Label = FString::Printf(TEXT("Road_%d"), BakedCount);
        // Slight sanitization for asset names
        Label.ReplaceInline(TEXT(" "), TEXT("_"));
        const FString AssetName = FString::Printf(TEXT("SM_%s"), *Label);

        UStaticMesh* BakedMesh = LevelToolMigrate::BakeProcMeshSectionToStaticMesh(
            PMC, 0, Mat, RoadsPkgRoot, AssetName);

        if (!BakedMesh)
        {
            Log(FString::Printf(TEXT("  ⚠ BakeRoads: failed for %s"), *Label));
            continue;
        }

        // Spawn replacement StaticMeshActor at same transform
        const FTransform Xform = RoadActor->GetActorTransform();
        FActorSpawnParameters P;
        P.bNoFail = true;
        AStaticMeshActor* SMA = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Xform.GetLocation(), Xform.Rotator(), P);
        if (SMA)
        {
            UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
            SMC->SetStaticMesh(BakedMesh);
            if (Mat) SMC->SetMaterial(0, Mat);
            SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            SMA->SetActorScale3D(Xform.GetScale3D());
            SMA->SetActorLabel(Label);
            SMA->SetFolderPath(TEXT("LevelTool/Roads"));
            SMA->Tags.AddUnique(TEXT("LevelToolGenerated"));
            SMA->Tags.AddUnique(TEXT("Baked"));
        }

        ToDestroy.Add(RoadActor);
        ++BakedCount;
    }

    for (AActor* A : ToDestroy)
    {
        A->Modify();
        World->DestroyActor(A);
    }

    GEditor->RedrawAllViewports();
    Log(FString::Printf(TEXT("✔ BakeRoads: %d actor(s) baked → %s"), BakedCount, *RoadsPkgRoot));
    return BakedCount;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CleanupForMigrate
// ─────────────────────────────────────────────────────────────────────────────
int32 ULevelToolSubsystem::CleanupForMigrate()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Log(TEXT("✖ CleanupForMigrate: no editor world"));
        return 0;
    }

    FScopedTransaction Transaction(
        NSLOCTEXT("LevelTool", "CleanupForMigrate", "LevelTool: Cleanup For Migrate"));

    TArray<AActor*> ToDelete;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) continue;

        const FString Folder = A->GetFolderPath().ToString();

        // 1) Compass markers + labels
        if (Folder.StartsWith(TEXT("LevelTool/Compass")))
        {
            ToDelete.Add(A);
            continue;
        }

        // 2) Stray TextRenderActors created by the tool
        if (A->IsA<ATextRenderActor>() && Folder.StartsWith(TEXT("LevelTool")))
        {
            ToDelete.Add(A);
            continue;
        }
    }

    for (AActor* A : ToDelete)
    {
        A->Modify();
        World->DestroyActor(A);
    }

    GEditor->RedrawAllViewports();
    Log(FString::Printf(TEXT("✔ CleanupForMigrate: removed %d debug actor(s)"), ToDelete.Num()));
    return ToDelete.Num();
}

// ─────────────────────────────────────────────────────────────────────────────
//  PackAssetsUnderMapData
// ─────────────────────────────────────────────────────────────────────────────
int32 ULevelToolSubsystem::PackAssetsUnderMapData(const FString& MapName)
{
    const FString CleanName = LevelToolMigrate::SanitizeMapName(MapName);

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    IAssetTools&    AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    AR.SearchAllAssets(true);

    const FString SrcRoot = TEXT("/Game/LevelTool");
    const FString DstRoot = FString::Printf(TEXT("/Game/MapData/%s/FromLevelTool"), *CleanName);

    TArray<FAssetData> Assets;
    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(*SrcRoot);
    AR.GetAssets(Filter, Assets);

    if (Assets.IsEmpty())
    {
        Log(FString::Printf(TEXT("ℹ PackAssets: nothing under %s"), *SrcRoot));
        return 0;
    }

    TArray<FAssetRenameData> Renames;
    Renames.Reserve(Assets.Num());

    for (const FAssetData& A : Assets)
    {
        UObject* Obj = A.GetAsset();
        if (!Obj) continue;

        // Rebase package path under the new root
        const FString OldPkg  = A.PackagePath.ToString();                // e.g. /Game/LevelTool/SplatMaps
        FString Relative      = OldPkg.RightChop(SrcRoot.Len());         //     /SplatMaps  (or empty)
        if (Relative.StartsWith(TEXT("/"))) Relative.RemoveAt(0);
        const FString NewPkgPath = Relative.IsEmpty() ? DstRoot : (DstRoot / Relative);

        FAssetRenameData Data;
        Data.Asset       = Obj;
        Data.NewPackagePath = NewPkgPath;
        Data.NewName     = A.AssetName.ToString();
        Renames.Add(Data);
    }

    const bool bOK = AT.RenameAssets(Renames);
    const int32 MovedCount = bOK ? Renames.Num() : 0;

    if (bOK)
    {
        Log(FString::Printf(TEXT("✔ PackAssets: %d asset(s) relocated → %s/..."),
            MovedCount, *DstRoot));
    }
    else
    {
        Log(TEXT("⚠ PackAssets: some assets may not have been renamed (check Output Log)"));
    }

    return MovedCount;
}
