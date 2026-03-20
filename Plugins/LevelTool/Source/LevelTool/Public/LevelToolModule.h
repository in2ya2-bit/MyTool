#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"

class FLevelToolModule : public IModuleInterface
{
public:
    virtual void StartupModule()  override;
    virtual void ShutdownModule() override;

    static const FName TabId;
    static const FName DesignerIntentTabId;

private:
    TSharedRef<SDockTab> OnSpawnTab(const FSpawnTabArgs& Args);
    TSharedRef<SDockTab> OnSpawnDesignerIntentTab(const FSpawnTabArgs& Args);
    void RegisterMenuEntry();
    void RegisterSettings();
};
