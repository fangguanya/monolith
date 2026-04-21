#pragma once

#include "Modules/ModuleManager.h"

class FMonolithPythonModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
