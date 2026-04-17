#include "MonolithPerfModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithPerfActions.h"

// LogMonolithPerf: DECLARE in MonolithPerfActions.h, DEFINE in MonolithPerfActions.cpp

void FMonolithPerfModule::StartupModule()
{
	LogCapture = new FPerfLogCapture();
	LogCapture->Install();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPerfActions::RegisterActions(Registry);

	int32 ActionCount = Registry.GetActions(TEXT("perf")).Num();
	UE_LOG(LogMonolithPerf, Log, TEXT("MonolithPerf: Loaded (%d actions in 'perf' namespace, log capture installed)"), ActionCount);
}

void FMonolithPerfModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("perf"));

	if (LogCapture)
	{
		LogCapture->Uninstall();
		delete LogCapture;
		LogCapture = nullptr;
	}
}

IMPLEMENT_MODULE(FMonolithPerfModule, MonolithPerf)
