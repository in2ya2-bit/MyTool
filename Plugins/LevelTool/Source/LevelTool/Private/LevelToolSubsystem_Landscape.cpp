#include "LevelToolSubsystem.h"
#include "LevelToolSettings.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "RenderingThread.h"

#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeEdit.h"
#include "LandscapeComponent.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"

#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Containers/Ticker.h"

#include "ScopedTransaction.h"
#include "EngineUtils.h"

bool ULevelToolSubsystem::ImportHeightmapAsLandscape(
    const FString& HeightmapPngPath, const FString& LandscapeName,
    float ElevationRangeM, float OverrideXYScaleCm)
{
    if (!FPaths::FileExists(HeightmapPngPath))
    {
        Log(FString::Printf(TEXT("✖ Heightmap not found: %s"), *HeightmapPngPath));
        return false;
    }

    const ULevelToolSettings* S = ULevelToolSettings::Get();
    const float XYScale = (OverrideXYScaleCm > 0.f) ? OverrideXYScaleCm : S->XYScaleCm;

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        Log(TEXT("✖ No editor world available"));
        return false;
    }

    int32 DetectedSize = S->HeightmapSize;
    {
        FString R16Check = FPaths::ChangeExtension(HeightmapPngPath, TEXT("r16"));
        if (FPaths::FileExists(R16Check))
        {
            int64 FileBytes = IFileManager::Get().FileSize(*R16Check);
            if (FileBytes > 0)
            {
                int32 NumPixels = static_cast<int32>(FileBytes / 2);
                int32 Side = FMath::RoundToInt32(FMath::Sqrt(static_cast<float>(NumPixels)));
                if (Side * Side == NumPixels && Side > 0)
                {
                    DetectedSize = Side;
                    if (DetectedSize != S->HeightmapSize)
                    {
                        Log(FString::Printf(TEXT("  ℹ Heightmap size detected from R16: %d×%d (settings: %d)"),
                            DetectedSize, DetectedSize, S->HeightmapSize));
                    }
                }
            }
        }
    }

    const int32 SectionSize       = 63;
    const int32 SectionsPerComp   = 1;
    const int32 QuadsPerComp      = SectionSize * SectionsPerComp;
    const int32 TargetSize        = DetectedSize - 1;
    const int32 ComponentCountX   = FMath::CeilToInt((float)TargetSize / QuadsPerComp);
    const int32 ComponentCountY   = ComponentCountX;

    const int32 NumVerts = DetectedSize * DetectedSize;
    TArray<uint16> HeightData;
    HeightData.SetNumUninitialized(NumVerts);

    FString R16Path = FPaths::ChangeExtension(HeightmapPngPath, TEXT("r16"));
    bool bLoadedFromR16 = false;

    if (FPaths::FileExists(R16Path))
    {
        TArray<uint8> RawR16;
        if (FFileHelper::LoadFileToArray(RawR16, *R16Path))
        {
            const int32 ExpectedBytes = NumVerts * sizeof(uint16);
            if (RawR16.Num() == ExpectedBytes)
            {
                FMemory::Memcpy(HeightData.GetData(), RawR16.GetData(), ExpectedBytes);
                bLoadedFromR16 = true;
                Log(FString::Printf(TEXT("  Loaded R16: %s  (%d bytes)"), *R16Path, RawR16.Num()));
            }
            else
            {
                Log(FString::Printf(TEXT("⚠ R16 size mismatch: got %d bytes, expected %d"),
                    RawR16.Num(), ExpectedBytes));
            }
        }
    }

    if (!bLoadedFromR16)
    {
        TArray<uint8> RawPng;
        if (!FFileHelper::LoadFileToArray(RawPng, *HeightmapPngPath))
        {
            Log(TEXT("✖ Failed to read heightmap file (neither R16 nor PNG found)"));
            return false;
        }

        IImageWrapperModule& IWM =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);

        if (!IW->SetCompressed(RawPng.GetData(), RawPng.Num()))
        {
            Log(TEXT("✖ Failed to decode PNG heightmap"));
            return false;
        }

        TArray<uint8> Uncompressed;
        if (!IW->GetRaw(ERGBFormat::Gray, 16, Uncompressed))
        {
            Log(TEXT("✖ Failed to get 16-bit grayscale from PNG"));
            return false;
        }

        if (Uncompressed.Num() < NumVerts * (int32)sizeof(uint16))
        {
            Log(FString::Printf(TEXT("✖ PNG data too small: %d bytes for %d verts"),
                Uncompressed.Num(), NumVerts));
            return false;
        }

        FMemory::Memcpy(HeightData.GetData(), Uncompressed.GetData(), NumVerts * sizeof(uint16));
        Log(TEXT("  Loaded PNG (16-bit fallback)"));
    }

    {
        uint16 MinH = 65535, MaxH = 0;
        for (int32 i = 0; i < HeightData.Num(); ++i)
        {
            MinH = FMath::Min(MinH, HeightData[i]);
            MaxH = FMath::Max(MaxH, HeightData[i]);
        }
        Log(FString::Printf(TEXT("  HeightData uint16 range: min=%u  max=%u  (flat if both=32768)"), MinH, MaxH));
    }

    const float ActualRangeM = (ElevationRangeM > 1.f)
        ? FMath::Clamp(ElevationRangeM, 1.f, 400.f)
        : S->ZRangeMeters;
    const float ZScale = ActualRangeM * 100.f / 512.f;
    const float ActorZ = 32768.f * ZScale / 128.f;

    const float HalfSizeCm = (DetectedSize - 1) * XYScale / 2.0f;
    const float ActorX     = -HalfSizeCm;
    const float ActorY     = -HalfSizeCm;

    FScopedTransaction Transaction(NSLOCTEXT("LevelTool", "ImportLandscape", "LevelTool: Import Landscape"));

    FActorSpawnParameters SpawnParams;
    SpawnParams.bNoFail = true;

    ALandscape* Landscape = World->SpawnActor<ALandscape>(
        ALandscape::StaticClass(), FVector(ActorX, ActorY, ActorZ), FRotator::ZeroRotator, SpawnParams);
    if (!Landscape)
    {
        Log(TEXT("✖ Failed to spawn Landscape actor"));
        return false;
    }

    Landscape->SetActorRelativeScale3D(FVector(XYScale, XYScale, ZScale));
    Landscape->SetActorLabel(LandscapeName);
    Landscape->Tags.AddUnique(TEXT("LevelToolGenerated"));

    Log(FString::Printf(TEXT("  ElevationRangeM: %.2fm  ZScale: %.4f  ActorXY: (%.0f, %.0f)  ActorZ: %.0fcm"),
        ActualRangeM, ZScale, ActorX, ActorY, ActorZ));

    TArray<uint16> HeightDataCopy = HeightData;
    const int32    HMapWidth      = DetectedSize;

    TMap<FGuid, TArray<uint16>> HeightDataMap;
    HeightDataMap.Add(FGuid(), MoveTemp(HeightData));

    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> EmptyLayerInfos;
    EmptyLayerInfos.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

    TArray<FLandscapeLayer> EmptyLayers;

    Landscape->Import(
        FGuid::NewGuid(),
        0, 0,
        ComponentCountX * QuadsPerComp,
        ComponentCountY * QuadsPerComp,
        SectionsPerComp,
        SectionSize,
        HeightDataMap,
        nullptr,
        EmptyLayerInfos,
        ELandscapeImportAlphamapType::Additive,
        TArrayView<const FLandscapeLayer>(EmptyLayers)
    );

    {
        ULandscapeInfo* LInfo = Landscape->GetLandscapeInfo();
        if (LInfo)
        {
            LInfo->ForAllLandscapeComponents([&](ULandscapeComponent* Comp)
            {
                UTexture2D* HTex = Comp->GetHeightmap(false);
                if (!HTex || !HTex->GetPlatformData()) return;

                FTexture2DMipMap& Mip = HTex->GetPlatformData()->Mips[0];
                if (Mip.BulkData.GetBulkDataSize() == 0) return;

                const int32 TexW = Mip.SizeX;
                const int32 TexH = Mip.SizeY;

                const int32 OffX = FMath::RoundToInt32(
                    static_cast<float>(Comp->HeightmapScaleBias.Z) * TexW);
                const int32 OffY = FMath::RoundToInt32(
                    static_cast<float>(Comp->HeightmapScaleBias.W) * TexH);

                const int32   CompVerts = Comp->ComponentSizeQuads + 1;
                const FIntPoint Base    = Comp->GetSectionBase();

                FColor* Pixels = static_cast<FColor*>(
                    Mip.BulkData.Lock(LOCK_READ_WRITE));
                if (!Pixels)
                {
                    Mip.BulkData.Unlock();
                    return;
                }

                for (int32 LY = 0; LY < CompVerts; ++LY)
                {
                    for (int32 LX = 0; LX < CompVerts; ++LX)
                    {
                        const int32 DataX   = Base.X + LX;
                        const int32 DataY   = Base.Y + LY;
                        const int32 DataIdx = DataY * HMapWidth + DataX;
                        if (DataIdx < 0 || DataIdx >= HeightDataCopy.Num()) continue;

                        const uint16 H    = HeightDataCopy[DataIdx];
                        const int32  PixX = OffX + LX;
                        const int32  PixY = OffY + LY;
                        if (PixX < 0 || PixX >= TexW || PixY < 0 || PixY >= TexH) continue;

                        FColor& P = Pixels[PixY * TexW + PixX];
                        P.R = static_cast<uint8>(H >> 8);
                        P.G = static_cast<uint8>(H & 0xFF);
                        P.B = 128;
                        P.A = 128;
                    }
                }

                Mip.BulkData.Unlock();
                HTex->UpdateResource();
                Comp->MarkRenderStateDirty();
            });

            Log(TEXT("  ✔ HeightmapTexture written directly — terrain should be visible"));
        }
    }

    CachedHeightData  = HeightDataCopy;
    CachedHMapWidth   = HMapWidth;
    CachedZScale      = ZScale;
    CachedXYScaleCm   = XYScale;
    CachedOriginX     = ActorX;
    CachedOriginY     = ActorY;

    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (LandscapeInfo)
    {
        LandscapeInfo->UpdateLayerInfoMap(Landscape);
        ULandscapeInfo::RecreateLandscapeInfo(World, true);
    }

    GEditor->SelectNone(false, true);
    GEditor->SelectActor(Landscape, true, true);
    GEditor->NoteSelectionChange();
    GEditor->RedrawAllViewports();

    Log(FString::Printf(TEXT("✔ Landscape created: %s  scale=%.0f×%.0f×%.4f  components=%d×%d"),
        *LandscapeName,
        XYScale, XYScale, ZScale,
        ComponentCountX, ComponentCountY));

    ImportSplatMapsAsTextures(HeightmapPngPath, Landscape);

    FBox LandscapeBounds = Landscape->GetComponentsBoundingBox(true);
    SpawnCompassMarkers(World, LandscapeBounds);

    return true;
}

void ULevelToolSubsystem::ImportSplatMapsAsTextures(const FString& HeightmapPngPath,
                                                     ALandscape* Landscape)
{
    const FString Dir    = FPaths::GetPath(HeightmapPngPath);
    FString       Prefix = FPaths::GetBaseFilename(HeightmapPngPath)
                               .Replace(TEXT("_heightmap"), TEXT(""));

    FString ColormapPath = FPaths::Combine(Dir,
        FString::Printf(TEXT("%s_colormap.png"), *Prefix));

    const TArray<FString> Layers = { TEXT("grass"), TEXT("rock"), TEXT("sand"), TEXT("snow") };
    TArray<FString> FilesToImport;

    if (FPaths::FileExists(ColormapPath))
        FilesToImport.Add(ColormapPath);

    for (const FString& Layer : Layers)
    {
        FString SplatPath = FPaths::Combine(Dir,
            FString::Printf(TEXT("%s_splat_%s.png"), *Prefix, *Layer));
        if (FPaths::FileExists(SplatPath))
            FilesToImport.Add(SplatPath);
    }

    if (FilesToImport.IsEmpty())
    {
        Log(TEXT("  ℹ No splat maps found — skipping texture import"));
        return;
    }

    Log(FString::Printf(TEXT("  ℹ Splat maps found (%d) — importing on next frame..."), FilesToImport.Num()));

    TWeakObjectPtr<ULevelToolSubsystem> WeakThis(this);
    TWeakObjectPtr<ALandscape> WeakLandscape(Landscape);

    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, WeakLandscape, FilesToImport, Prefix](float) -> bool
        {
            ULevelToolSubsystem* Self = WeakThis.Get();
            ALandscape* Land = WeakLandscape.Get();
            if (!Self) return false;

            IAssetTools& AssetTools =
                FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

            UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
            ImportData->DestinationPath  = TEXT("/Game/LevelTool/SplatMaps");
            ImportData->Filenames        = FilesToImport;
            ImportData->bReplaceExisting = true;

            TArray<UObject*> Imported = AssetTools.ImportAssetsAutomated(ImportData);

            Self->Log(FString::Printf(
                TEXT("  ✔ Splat maps imported: %d textures → /Game/LevelTool/SplatMaps/"),
                Imported.Num()));

            if (Land)
            {
                FString ColormapAsset = FString::Printf(
                    TEXT("/Game/LevelTool/SplatMaps/%s_colormap.%s_colormap"), *Prefix, *Prefix);
                UTexture2D* ColormapTex = LoadObject<UTexture2D>(nullptr, *ColormapAsset);

                if (ColormapTex)
                {
                    static const TCHAR* MatPath = TEXT("/Game/LevelTool/M_LandscapeAuto.M_LandscapeAuto");
                    UMaterial* LandMat = LoadObject<UMaterial>(nullptr, MatPath);

                    if (!LandMat)
                    {
                        FString PkgPath = TEXT("/Game/LevelTool/M_LandscapeAuto");
                        UPackage* Pkg = CreatePackage(*PkgPath);
                        Pkg->FullyLoad();

                        LandMat = NewObject<UMaterial>(Pkg, TEXT("M_LandscapeAuto"), RF_Public | RF_Standalone);

                        auto* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(LandMat);
                        TexCoord->CoordinateIndex = 0;

                        auto* TexSample = NewObject<UMaterialExpressionTextureSampleParameter2D>(LandMat);
                        TexSample->ParameterName = TEXT("Colormap");
                        TexSample->Texture = ColormapTex;
                        TexSample->SamplerType = SAMPLERTYPE_Color;
                        TexSample->Coordinates.Connect(0, TexCoord);

                        auto* RoughParam = NewObject<UMaterialExpressionScalarParameter>(LandMat);
                        RoughParam->ParameterName = TEXT("Roughness");
                        RoughParam->DefaultValue  = 0.85f;

                        auto& Exprs = LandMat->GetExpressionCollection().Expressions;
                        Exprs.Add(TexCoord);
                        Exprs.Add(TexSample);
                        Exprs.Add(RoughParam);

                        auto* Ed = LandMat->GetEditorOnlyData();
                        Ed->BaseColor.Connect(0, TexSample);
                        Ed->Roughness.Connect(0, RoughParam);

                        LandMat->PreEditChange(nullptr);
                        LandMat->PostEditChange();

                        FAssetRegistryModule::AssetCreated(LandMat);
                        Pkg->MarkPackageDirty();
                        FString FilePath = FPackageName::LongPackageNameToFilename(
                            PkgPath, FPackageName::GetAssetPackageExtension());
                        FSavePackageArgs SaveArgs;
                        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                        UPackage::SavePackage(Pkg, LandMat, *FilePath, SaveArgs);

                        Self->Log(TEXT("  ✔ Landscape material M_LandscapeAuto created"));
                    }

                    Land->LandscapeMaterial = LandMat;
                    Land->MarkPackageDirty();
                    Land->PostEditChange();
                    Self->Log(TEXT("  ✔ Landscape material applied (colormap-based)"));
                }
                else
                {
                    Self->Log(TEXT("  ⚠ Colormap texture not found after import — assign splat maps manually"));
                }
            }

            return false;
        })
    );
}

void ULevelToolSubsystem::SpawnCompassMarkers(UWorld* World, const FBox& LandscapeBounds)
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UMaterial*   BaseMat  = LoadObject<UMaterial>(nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!CubeMesh || !BaseMat)
    {
        Log(TEXT("  ⚠ Compass: could not load BasicShapes assets"));
        return;
    }

    const FString Folder = TEXT("LevelTool/Compass");

    float DiagCm     = (LandscapeBounds.Max - LandscapeBounds.Min).Size2D();
    float PillarH    = FMath::Max(DiagCm * 0.08f, 3000.0f);
    float PillarW    = FMath::Max(DiagCm * 0.003f, 80.0f);
    float BaseZ      = LandscapeBounds.Max.Z;
    float TextSize   = FMath::Max(PillarH * 0.15f, 500.0f);

    FVector Center2D = FVector(
        (LandscapeBounds.Min.X + LandscapeBounds.Max.X) * 0.5f,
        (LandscapeBounds.Min.Y + LandscapeBounds.Max.Y) * 0.5f,
        0.f);

    auto SpawnMarker = [&](FVector XY_Pos, FLinearColor Color, const FString& Label, const FString& DirText)
    {
        FVector SpawnLoc(XY_Pos.X, XY_Pos.Y, BaseZ + PillarH * 0.5f);

        FActorSpawnParameters P;
        P.bNoFail = true;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), SpawnLoc, FRotator::ZeroRotator, P);
        if (!Actor) return;

        UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
        Comp->SetStaticMesh(CubeMesh);
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(BaseMat, Actor);
        Mat->SetVectorParameterValue(TEXT("Color"), FVector4(Color.R, Color.G, Color.B, 1.0f));
        Comp->SetMaterial(0, Mat);

        Actor->SetActorScale3D(FVector(PillarW / 100.f, PillarW / 100.f, PillarH / 100.f));
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(*Folder);

        FVector TextLoc(XY_Pos.X, XY_Pos.Y, BaseZ + PillarH + TextSize);
        FActorSpawnParameters TextParams;
        TextParams.bNoFail = true;
        ATextRenderActor* TextActor = World->SpawnActor<ATextRenderActor>(
            ATextRenderActor::StaticClass(), TextLoc, FRotator::ZeroRotator, TextParams);
        if (TextActor)
        {
            UTextRenderComponent* TC = TextActor->GetTextRender();
            TC->SetText(FText::FromString(DirText));
            TC->SetTextRenderColor(Color.ToFColor(true));
            TC->SetWorldSize(TextSize);
            TC->SetHorizontalAlignment(EHTA_Center);
            TC->SetVerticalAlignment(EVRTA_TextCenter);
            TextActor->SetActorLabel(Label + TEXT("_Label"));
            TextActor->SetFolderPath(*Folder);
        }
    };

    float EdgeOffset = FMath::Min(
        (LandscapeBounds.Max.X - LandscapeBounds.Min.X) * 0.45f,
        (LandscapeBounds.Max.Y - LandscapeBounds.Min.Y) * 0.45f);

    SpawnMarker(Center2D, FLinearColor(1.f, 0.f, 0.f), TEXT("Compass_CENTER"), TEXT("CENTER"));
    SpawnMarker(Center2D + FVector(0, -EdgeOffset, 0), FLinearColor(0.f, 0.4f, 1.f), TEXT("Compass_N"), TEXT("N"));
    SpawnMarker(Center2D + FVector(0, +EdgeOffset, 0), FLinearColor(1.f, 0.9f, 0.f), TEXT("Compass_S"), TEXT("S"));
    SpawnMarker(Center2D + FVector(+EdgeOffset, 0, 0), FLinearColor(0.f, 0.9f, 0.f), TEXT("Compass_E"), TEXT("E"));
    SpawnMarker(Center2D + FVector(-EdgeOffset, 0, 0), FLinearColor(1.f, 0.5f, 0.f), TEXT("Compass_W"), TEXT("W"));

    Log(TEXT("  ✔ Compass markers spawned with labels (LevelTool/Compass)"));
}

float ULevelToolSubsystem::GetTerrainZAtWorldXY(float WorldX, float WorldY) const
{
    if (CachedHeightData.IsEmpty() || CachedHMapWidth <= 0 || CachedXYScaleCm <= 0.f)
        return 0.f;

    const int32 PixX = FMath::Clamp(FMath::RoundToInt32((WorldX - CachedOriginX) / CachedXYScaleCm), 0, CachedHMapWidth - 1);
    const int32 PixY = FMath::Clamp(FMath::RoundToInt32((WorldY - CachedOriginY) / CachedXYScaleCm), 0, CachedHMapWidth - 1);
    const uint16 H   = CachedHeightData[PixY * CachedHMapWidth + PixX];
    return CachedZScale * static_cast<float>(H) / 128.0f;
}
