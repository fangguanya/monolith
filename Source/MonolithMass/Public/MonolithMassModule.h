// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * MonolithMass MCP module.
 *
 * At StartupModule() this will register four MCP namespaces into
 * FMonolithToolRegistry once Phase C action classes land:
 *
 *   - zg.*          (~12 tools) — ZoneGraph authoring / queries / build.
 *   - masstraffic.* (~10 tools) — CitySample MassTraffic runtime + settings.
 *   - masscrowd.*   (~8 tools)  — Engine MassCrowd runtime + density + settings.
 *   - mass.*        (~8 tools)  — Generic Mass Entity subsystem tooling.
 *
 * Each namespace registration is guarded by its corresponding WITH_* define so
 * the module degrades gracefully when a host project is missing one of the
 * dependency plugins (see MonolithMass.Build.cs conditional probe).
 */
class FMonolithMassModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
