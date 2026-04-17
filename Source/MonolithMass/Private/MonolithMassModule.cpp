// Copyright Matrix Team. All Rights Reserved.

#include "MonolithMassModule.h"
#include "MonolithMassLog.h"
#include "MonolithToolRegistry.h"

#include "Actions/MonolithZoneGraphActions.h"
#include "Actions/MonolithMassTrafficActions.h"
#include "Actions/MonolithMassCrowdActions.h"
// NOTE: mass.* (MonolithMassCoreActions) intentionally removed — overlapping with
// existing ai.list_mass_processors / ai.get_mass_entity_stats from MonolithAI.
// See Wiki/.../Tools/06_MonolithMass_MCP.md §"Why no mass.*" for the rationale.

DEFINE_LOG_CATEGORY(LogMonolithMass);

#define LOCTEXT_NAMESPACE "FMonolithMassModule"

void FMonolithMassModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Each namespace's RegisterActions is gated on its own WITH_* define, so calling all four
	// unconditionally is safe — missing plugins simply skip registration and log a note.
	FMonolithZoneGraphActions::RegisterActions(Registry);
	FMonolithMassTrafficActions::RegisterActions(Registry);
	FMonolithMassCrowdActions::RegisterActions(Registry);

	const int32 ZgCount = Registry.GetActions(TEXT("zg")).Num();
	const int32 TrafficCount = Registry.GetActions(TEXT("masstraffic")).Num();
	const int32 CrowdCount = Registry.GetActions(TEXT("masscrowd")).Num();

	const TCHAR* ZgStatus =
#if WITH_ZONEGRAPH
		TEXT("available");
#else
		TEXT("missing");
#endif
	const TCHAR* MassStatus =
#if WITH_MASSENTITY
		TEXT("available");
#else
		TEXT("missing");
#endif
	const TCHAR* CrowdStatus =
#if WITH_MASSCROWD
		TEXT("available");
#else
		TEXT("missing");
#endif
	const TCHAR* TrafficStatus =
#if WITH_MASSTRAFFIC
		TEXT("available");
#else
		TEXT("missing");
#endif

	UE_LOG(LogMonolithMass, Log,
		TEXT("MonolithMass loaded: zg=%d masstraffic=%d masscrowd=%d "
		     "(ZoneGraph=%s, MassEntity=%s, MassCrowd=%s, MassTraffic=%s) "
		     "[mass.* intentionally absent — use MonolithAI's ai.list_mass_processors / ai.get_mass_entity_stats]"),
		ZgCount, TrafficCount, CrowdCount,
		ZgStatus, MassStatus, CrowdStatus, TrafficStatus);
}

void FMonolithMassModule::ShutdownModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.UnregisterNamespace(TEXT("zg"));
	Registry.UnregisterNamespace(TEXT("masstraffic"));
	Registry.UnregisterNamespace(TEXT("masscrowd"));

	UE_LOG(LogMonolithMass, Log, TEXT("MonolithMass: Shutdown complete"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMassModule, MonolithMass)
