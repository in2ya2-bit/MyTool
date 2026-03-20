#include "LevelToolModule.h"
#include "LevelToolSettings.h"
#include "SLevelToolPanel.h"
#include "SDesignerIntentPanel.h"

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
const FName FLevelToolModule::DesignerIntentTabId = FName("DesignerIntentTab");

// ─────────────────────────────────────────────────────────────────────────────
//  Startup / Shutdown
// ─────────────────────────────────────────────────────────────────────────────

void FLevelToolModule::StartupModule()
{
    // Register tab spawner — Stage 1 맵 생성
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        TabId,
        FOnSpawnTab::CreateRaw(this, &FLevelToolModule::OnSpawnTab))
        .SetDisplayName(LOCTEXT("TabTitle", "Level Tool"))
        .SetTooltipText(LOCTEXT("TabTooltip", "Real-world map generator for UE5"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

    // Register tab spawner — Stage 2~3 Designer Intent
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        DesignerIntentTabId,
        FOnSpawnTab::CreateRaw(this, &FLevelToolModule::OnSpawnDesignerIntentTab))
        .SetDisplayName(LOCTEXT("DITabTitle", "Designer Intent"))
        .SetTooltipText(LOCTEXT("DITabTooltip", "Modify map with designer intent sliders and presets"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

    RegisterMenuEntry();
    RegisterSettings();
}

void FLevelToolModule::ShutdownModule()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DesignerIntentTabId);
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

TSharedRef<SDockTab> FLevelToolModule::OnSpawnDesignerIntentTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
    .TabRole(ETabRole::NomadTab)
    .Label(LOCTEXT("DITabLabel","Designer Intent"))
    [
        SNew(SDesignerIntentPanel)
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

            Section.AddMenuEntry(
                TEXT("OpenDesignerIntent"),
                LOCTEXT("OpenDesignerIntent",   "Designer Intent"),
                LOCTEXT("OpenDesignerIntentTip","Open the designer intent modification panel"),
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
                FUIAction(FExecuteAction::CreateLambda([this]()
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(DesignerIntentTabId);
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
