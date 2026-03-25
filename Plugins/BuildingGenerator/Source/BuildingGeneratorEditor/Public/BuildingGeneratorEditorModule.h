#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"

class FBuildingGeneratorEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    static const FName TabId;

private:
    TSharedRef<SDockTab> OnSpawnTab(const FSpawnTabArgs& Args);
    void RegisterMenuEntry();
};
