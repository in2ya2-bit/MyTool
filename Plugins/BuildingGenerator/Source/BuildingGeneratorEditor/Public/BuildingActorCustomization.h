#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FBuildingActorCustomization : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
    TArray<TWeakObjectPtr<UObject>> SelectedObjects;
};
