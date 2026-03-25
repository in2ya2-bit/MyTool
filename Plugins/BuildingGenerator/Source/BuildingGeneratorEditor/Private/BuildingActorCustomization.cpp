#include "BuildingActorCustomization.h"
#include "BuildingActor.h"
#include "BuildingDataAsset.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "ScopedTransaction.h"
#include "Misc/MessageDialog.h"
#include "Framework/Docking/TabManager.h"
#include "AssetToolsModule.h"
#include "Factories/DataAssetFactory.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "BuildingActorCustomization"

TSharedRef<IDetailCustomization> FBuildingActorCustomization::MakeInstance()
{
    return MakeShareable(new FBuildingActorCustomization);
}

void FBuildingActorCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    DetailBuilder.GetObjectsBeingCustomized(SelectedObjects);

    IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(
        "Building", LOCTEXT("Cat", "Building Generator"), ECategoryPriority::Important);

    // ── Rebuild HISM ──
    Cat.AddCustomRow(LOCTEXT("RebuildRow", "Rebuild"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("RebuildLabel", "HISM Rendering"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(200.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("RebuildBtn", "Rebuild HISM"))
            .ToolTipText(LOCTEXT("RebuildTip",
                "Clear and regenerate all HISM instances from tile data"))
            .HAlign(HAlign_Center)
            .OnClicked_Lambda([this]()
            {
                for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                {
                    if (ABuildingActor* A = Cast<ABuildingActor>(Obj.Get()))
                    {
                        FScopedTransaction Txn(LOCTEXT("RebuildTxn",
                            "BuildingGenerator: Rebuild HISM"));
                        A->Modify();
                        A->RebuildHISM();
                    }
                }
                return FReply::Handled();
            })
        ];

    // ── Open Tilemap Editor ──
    Cat.AddCustomRow(LOCTEXT("EditorRow", "Editor"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("EditorLabel", "Tile Editor"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(200.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("OpenEditorBtn", "Open Building Editor"))
            .ToolTipText(LOCTEXT("OpenEditorTip",
                "Open the tilemap editor to paint tiles on each floor"))
            .HAlign(HAlign_Center)
            .OnClicked_Lambda([]()
            {
                FGlobalTabmanager::Get()->TryInvokeTab(FName("BuildingEditorTab"));
                return FReply::Handled();
            })
        ];

    // ── Validate MeshSet ──
    Cat.AddCustomRow(LOCTEXT("ValidateRow", "Validate"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("ValidateLabel", "Validation"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(200.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("ValidateBtn", "Validate MeshSet"))
            .HAlign(HAlign_Center)
            .OnClicked_Lambda([this]()
            {
                for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                {
                    ABuildingActor* A = Cast<ABuildingActor>(Obj.Get());
                    if (!A) continue;

                    if (!A->MeshSet)
                    {
                        FMessageDialog::Open(EAppMsgType::Ok,
                            LOCTEXT("NoMeshSet", "No MeshSet assigned."));
                        continue;
                    }

                    TArray<FString> Warnings = A->MeshSet->Validate();
                    if (Warnings.IsEmpty())
                    {
                        FMessageDialog::Open(EAppMsgType::Ok,
                            LOCTEXT("ValidateOk", "MeshSet validation passed."));
                    }
                    else
                    {
                        FString Msg = TEXT("Warnings:\n");
                        for (const FString& W : Warnings)
                            Msg += FString::Printf(TEXT("  - %s\n"), *W);
                        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Msg));
                    }
                }
                return FReply::Handled();
            })
        ];

    // ── Apply Presets ──
    Cat.AddCustomRow(LOCTEXT("PresetRow", "Preset"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PresetLabel", "Preset"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(300.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0, 0, 4, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("Preset1FBtn", "1-Story"))
                .ToolTipText(LOCTEXT("Preset1FTip",
                    "1F: door, 2 rooms, 2 windows, hall"))
                .HAlign(HAlign_Center)
                .OnClicked_Lambda([this]()
                {
                    for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                    {
                        ABuildingActor* A = Cast<ABuildingActor>(Obj.Get());
                        if (!A) continue;

                        FScopedTransaction Txn(LOCTEXT("Preset1FTxn",
                            "BuildingGenerator: Apply 1-Story Preset"));
                        A->Modify();
                        A->ApplyPreset_OneStoryBuilding();
                        A->RebuildHISM();
                    }
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("Preset2FBtn", "2-Story"))
                .ToolTipText(LOCTEXT("Preset2FTip",
                    "2F: door, 2 rooms/floor, 2 windows/floor, stairs"))
                .HAlign(HAlign_Center)
                .OnClicked_Lambda([this]()
                {
                    for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                    {
                        ABuildingActor* A = Cast<ABuildingActor>(Obj.Get());
                        if (!A) continue;

                        FScopedTransaction Txn(LOCTEXT("Preset2FTxn",
                            "BuildingGenerator: Apply 2-Story Preset"));
                        A->Modify();
                        A->ApplyPreset_TwoStoryBuilding();
                        A->RebuildHISM();
                    }
                    return FReply::Handled();
                })
            ]
        ];

    // ── Auto-Populate Meshes ──
    Cat.AddCustomRow(LOCTEXT("PopulateRow", "Populate"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PopulateLabel", "Mesh Setup"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(200.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("PopulateBtn", "Auto-Populate Meshes"))
            .ToolTipText(LOCTEXT("PopulateTip",
                "Scan /Game/BuildingGenerator/Meshes and auto-assign all matching meshes to this DataAsset"))
            .HAlign(HAlign_Center)
            .OnClicked_Lambda([this]()
            {
                for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                {
                    ABuildingActor* A = Cast<ABuildingActor>(Obj.Get());
                    if (!A) continue;

                    if (!A->MeshSet)
                    {
                        FMessageDialog::Open(EAppMsgType::Ok,
                            LOCTEXT("PopulateNoDA",
                                "No MeshSet assigned.\nAssign or create a BuildingDataAsset first."));
                        continue;
                    }

                    FScopedTransaction Txn(LOCTEXT("PopulateTxn",
                        "BuildingGenerator: Auto-Populate Meshes"));
                    A->MeshSet->Modify();

                    FString Dir = A->MeshSet->MeshSourceDirectory.Path;
                    if (Dir.IsEmpty())
                    {
                        Dir = TEXT("/Game/BuildingGenerator/Meshes");
                    }

                    int32 Count = A->MeshSet->AutoPopulateFromDirectory(Dir);

                    A->MeshSet->MarkPackageDirty();

                    FString Msg = FString::Printf(
                        TEXT("Assigned %d mesh slots from:\n%s\n\nSave the DataAsset to persist changes."),
                        Count, *Dir);
                    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Msg));
                }
                return FReply::Handled();
            })
        ];

    // ── Create DataAsset ──
    Cat.AddCustomRow(LOCTEXT("CreateDARow", "Create"))
        .NameContent()
        [
            SNew(STextBlock)
            .Text(LOCTEXT("CreateDALabel", "DataAsset"))
            .Font(IDetailLayoutBuilder::GetDetailFont())
        ]
        .ValueContent()
        .MaxDesiredWidth(200.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("CreateDABtn", "Create & Assign DataAsset"))
            .ToolTipText(LOCTEXT("CreateDATip",
                "Create a new BuildingDataAsset and assign it to this actor"))
            .HAlign(HAlign_Center)
            .OnClicked_Lambda([this]()
            {
                for (TWeakObjectPtr<UObject>& Obj : SelectedObjects)
                {
                    ABuildingActor* A = Cast<ABuildingActor>(Obj.Get());
                    if (!A) continue;

                    if (A->MeshSet)
                    {
                        EAppReturnType::Type Result = FMessageDialog::Open(
                            EAppMsgType::YesNo,
                            LOCTEXT("OverwriteDA",
                                "MeshSet is already assigned.\nCreate a new one and replace it?"));
                        if (Result != EAppReturnType::Yes)
                            continue;
                    }

                    FString PackagePath = TEXT("/Game/BuildingGenerator");
                    FString AssetName = TEXT("DA_BuildingAsset");

                    IAssetTools& AssetTools =
                        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools")
                        .Get();

                    UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();

                    UObject* NewAsset = AssetTools.CreateAsset(
                        AssetName, PackagePath, UBuildingDataAsset::StaticClass(), Factory);

                    UBuildingDataAsset* NewDA = Cast<UBuildingDataAsset>(NewAsset);
                    if (!NewDA)
                    {
                        FMessageDialog::Open(EAppMsgType::Ok,
                            LOCTEXT("CreateFail", "Failed to create DataAsset."));
                        continue;
                    }

                    FScopedTransaction Txn(LOCTEXT("CreateDATxn",
                        "BuildingGenerator: Create & Assign DataAsset"));
                    A->Modify();
                    A->MeshSet = NewDA;

                    FMessageDialog::Open(EAppMsgType::Ok,
                        FText::FromString(FString::Printf(
                            TEXT("Created and assigned:\n%s/%s\n\nUse 'Auto-Populate Meshes' to fill it."),
                            *PackagePath, *AssetName)));
                }
                return FReply::Handled();
            })
        ];
}

#undef LOCTEXT_NAMESPACE
