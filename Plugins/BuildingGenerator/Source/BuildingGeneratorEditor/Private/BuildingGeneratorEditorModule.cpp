#include "BuildingGeneratorEditorModule.h"
#include "SBuildingEditorWindow.h"
#include "BuildingActorCustomization.h"
#include "BuildingActor.h"

#include "Framework/Docking/TabManager.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "BuildingGeneratorEditor"

IMPLEMENT_MODULE(FBuildingGeneratorEditorModule, BuildingGeneratorEditor)

const FName FBuildingGeneratorEditorModule::TabId = FName("BuildingEditorTab");

void FBuildingGeneratorEditorModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        TabId,
        FOnSpawnTab::CreateRaw(this, &FBuildingGeneratorEditorModule::OnSpawnTab))
        .SetDisplayName(LOCTEXT("TabTitle", "Building Editor"))
        .SetTooltipText(LOCTEXT("TabTooltip", "Tile-based building editor with AutoTile"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

    RegisterMenuEntry();

    FPropertyEditorModule& PropertyModule =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    PropertyModule.RegisterCustomClassLayout(
        ABuildingActor::StaticClass()->GetFName(),
        FOnGetDetailCustomizationInstance::CreateStatic(
            &FBuildingActorCustomization::MakeInstance));
}

void FBuildingGeneratorEditorModule::ShutdownModule()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
    UToolMenus::UnregisterOwner(this);

    if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
    {
        FPropertyEditorModule& PropertyModule =
            FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyModule.UnregisterCustomClassLayout(
            ABuildingActor::StaticClass()->GetFName());
    }
}

TSharedRef<SDockTab> FBuildingGeneratorEditorModule::OnSpawnTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        .Label(LOCTEXT("TabLabel", "Building Editor"))
        [
            SNew(SBuildingEditorWindow)
        ];
}

void FBuildingGeneratorEditorModule::RegisterMenuEntry()
{
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
        {
            UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(
                "LevelEditor.MainMenu.Tools");
            FToolMenuSection& Section = ToolsMenu->FindOrAddSection(
                TEXT("BuildingGeneratorSection"),
                LOCTEXT("MenuSection", "Building Generator"));

            Section.AddMenuEntry(
                TEXT("OpenBuildingEditor"),
                LOCTEXT("MenuOpen", "Building Editor"),
                LOCTEXT("MenuOpenTip", "Open the tile-based building editor"),
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
                FUIAction(FExecuteAction::CreateLambda([this]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(TabId);
                }))
            );
        })
    );
}

#undef LOCTEXT_NAMESPACE
