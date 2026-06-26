#pragma once
#include "Modules/ModuleManager.h"

class FCrowdyNetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
