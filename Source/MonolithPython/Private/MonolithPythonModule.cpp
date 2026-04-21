#include "MonolithPythonModule.h"
#include "MonolithPythonActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithPythonModule"

void FMonolithPythonModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnablePython) return;

	FMonolithPythonActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolith, Log, TEXT("Monolith — Python module loaded (3 actions)"));
}

void FMonolithPythonModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("python"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithPythonModule, MonolithPython)
