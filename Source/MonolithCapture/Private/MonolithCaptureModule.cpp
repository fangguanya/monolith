#include "MonolithCaptureModule.h"
#include "MonolithCaptureActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithCapture);

void FMonolithCaptureModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableCapture)
	{
		UE_LOG(LogMonolithCapture, Log, TEXT("MonolithCapture: Capture module disabled in settings"));
		return;
	}

	FMonolithCaptureActions::RegisterActions();

	int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("capture")).Num();
	UE_LOG(LogMonolithCapture, Log, TEXT("MonolithCapture: Loaded (%d actions)"), ActionCount);
}

void FMonolithCaptureModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("capture"));
}

IMPLEMENT_MODULE(FMonolithCaptureModule, MonolithCapture)
