#include "LevelToolModule.h"
#include "LevelToolSettings.h"
#include "SLevelToolPanel.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "LevelTool"

IMPLEMENT_MODULE(FLevelToolModule, LevelTool)

const FName FLevelToolModule::TabId = FName("LevelToolTab");

// ─────────────────────────────────────────────────────────────────────────────
//  Startup / Shutdown
// ─────────────────────────────────────────────────────────────────────────────

void FLevelToolModule::StartupModule()
{
    // Register tab spawner
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        TabId,
        FOnSpawnTab::CreateRaw(this, &FLevelToolModule::OnSpawnTab))
        .SetDisplayName(LOCTEXT("TabTitle", "Level Tool"))
        .SetTooltipText(LOCTEXT("TabTooltip", "Real-world map generator for UE5"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

    RegisterMenuEntry();
    RegisterSettings();
}

void FLevelToolModule::ShutdownModule()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
    UToolMenus::UnregisterOwner(this);

    if (ISettingsModule* SM = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
    {
        SM->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("LevelTool"));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tab
// ─────────────────────────────────────────────────────────────────────────────

TSharedRef<SDockTab> FLevelToolModule::OnSpawnTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
    .TabRole(ETabRole::NomadTab)
    .Label(LOCTEXT("TabLabel","Level Tool"))
    [
        SNew(SLevelToolPanel)
    ];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Menu entry (Tools → Level Tool)
// ─────────────────────────────────────────────────────────────────────────────

void FLevelToolModule::RegisterMenuEntry()
{
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
        {
            UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
            FToolMenuSection& Section = ToolsMenu->FindOrAddSection(
                TEXT("LevelToolSection"),
                LOCTEXT("LevelToolMenuSection","Level Tool"));

            Section.AddMenuEntry(
                TEXT("OpenLevelTool"),
                LOCTEXT("OpenLevelTool",   "Level Tool"),
                LOCTEXT("OpenLevelToolTip","Open the real-world map generator panel"),
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
                FUIAction(FExecuteAction::CreateLambda([this]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(TabId);
                }))
            );
        })
    );
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project Settings
// ─────────────────────────────────────────────────────────────────────────────

void FLevelToolModule::RegisterSettings()
{
    if (ISettingsModule* SM = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
    {
        SM->RegisterSettings(
            TEXT("Project"),      // container
            TEXT("Plugins"),      // category
            TEXT("LevelTool"),    // section name
            LOCTEXT("SettingsName",        "Level Tool"),
            LOCTEXT("SettingsDescription", "Configure API keys and generation settings for the Level Tool plugin"),
            GetMutableDefault<ULevelToolSettings>()
        );
    }
}

#undef LOCTEXT_NAMESPACE
