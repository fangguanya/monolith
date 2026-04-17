#pragma once

#include "Modules/ModuleManager.h"

class FPerfLogCapture;

class FMonolithPerfModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FPerfLogCapture* LogCapture = nullptr;
};
