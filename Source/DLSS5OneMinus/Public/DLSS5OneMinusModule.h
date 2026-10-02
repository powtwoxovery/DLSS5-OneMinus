#pragma once

#include "Modules/ModuleInterface.h"

class FDLSS5OneMinusModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool SupportsDynamicReloading() override { return false; }
};
