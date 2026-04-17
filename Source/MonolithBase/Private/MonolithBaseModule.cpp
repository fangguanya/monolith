#include "MonolithBaseModule.h"
#include "MonolithBaseActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"

#include "Actions/MonolithBaseFunctionActions.h"
#include "Actions/MonolithPIEActions.h"
#include "Actions/MonolithConsoleActions.h"
#include "Actions/MonolithBakeActions.h"
#include "Actions/MonolithProjectConfigActions.h"

DEFINE_LOG_CATEGORY(LogMonolithBase);

void FMonolithBaseModule::StartupModule()
{
	// Existing 36 base.* actions.
	FMonolithBaseActions::RegisterActions();

	// New extensions added by the PCG-Mass integration CL — wired in here so all related
	// namespaces register from the same module entry-point and unregister together.
	FMonolithBaseFunctionActions::RegisterActions();   // base.invoke_function / call_in_editor_function
	FMonolithPIEActions::RegisterActions();            // pie.*
	FMonolithConsoleActions::RegisterActions();        // console.*
	FMonolithBakeActions::RegisterActions();           // bake.*
	FMonolithProjectConfigActions::RegisterActions();  // project.read/write/reload_config

	int32 BaseCount    = FMonolithToolRegistry::Get().GetActions(TEXT("base")).Num();
	int32 PieCount     = FMonolithToolRegistry::Get().GetActions(TEXT("pie")).Num();
	int32 ConsoleCount = FMonolithToolRegistry::Get().GetActions(TEXT("console")).Num();
	int32 BakeCount    = FMonolithToolRegistry::Get().GetActions(TEXT("bake")).Num();
	int32 ProjectCount = FMonolithToolRegistry::Get().GetActions(TEXT("project")).Num();
	UE_LOG(LogMonolithBase, Log,
		TEXT("MonolithBase loaded: base=%d pie=%d console=%d bake=%d project=%d"),
		BaseCount, PieCount, ConsoleCount, BakeCount, ProjectCount);
}

void FMonolithBaseModule::ShutdownModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	// "base" / "project" namespaces are shared with other modules — unregister only when the
	// host module is unloading entirely; per-action cleanup isn't strictly necessary for the
	// editor-only Monolith server.
	Registry.UnregisterNamespace(TEXT("base"));
	Registry.UnregisterNamespace(TEXT("pie"));
	Registry.UnregisterNamespace(TEXT("console"));
	Registry.UnregisterNamespace(TEXT("bake"));
	// `project` was originally registered by MonolithIndex; do not unregister here to avoid
	// nuking that namespace on partial reloads.
}

IMPLEMENT_MODULE(FMonolithBaseModule, MonolithBase)
