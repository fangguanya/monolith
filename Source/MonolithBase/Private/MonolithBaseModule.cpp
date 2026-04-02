#include "MonolithBaseModule.h"
#include "MonolithBaseActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithBase);

void FMonolithBaseModule::StartupModule()
{
	FMonolithBaseActions::RegisterActions();

	int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("base")).Num();
	UE_LOG(LogMonolithBase, Log, TEXT("MonolithBase: Loaded (%d actions)"), ActionCount);
}

void FMonolithBaseModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("base"));
}

IMPLEMENT_MODULE(FMonolithBaseModule, MonolithBase)
