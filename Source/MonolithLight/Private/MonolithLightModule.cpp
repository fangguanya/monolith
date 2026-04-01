#include "MonolithLightModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithLightActions.h"
#include "MonolithLightBuildActions.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithLight, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithLight);

void FMonolithLightModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableLight)
	{
		UE_LOG(LogMonolithLight, Log, TEXT("MonolithLight: Light module disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLightActions::RegisterActions(Registry);
	FMonolithLightBuildActions::RegisterActions(Registry);

	int32 ActionCount = Registry.GetActions(TEXT("light")).Num();
	UE_LOG(LogMonolithLight, Log, TEXT("MonolithLight: Loaded (%d actions)"), ActionCount);
}

void FMonolithLightModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("light"));
}

IMPLEMENT_MODULE(FMonolithLightModule, MonolithLight)
