// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * `bake.*` MCP namespace — unified entry-point for editor-time bake / build operations.
 *
 * Tools (3):
 *   - bake.zone_graph         : invoke FZoneGraphBuilder::BuildAll on every AZoneGraphData
 *   - bake.navmesh            : trigger NavSystem.Build
 *   - bake.lighting           : trigger Lightmass build (Editor only, slow)
 *
 * Each is best-effort and never blocks longer than the engine's underlying call.
 *
 * Note: `bake.zone_graph` overlaps with `zg.build_zone_graph` from MonolithMass — kept here as a
 * convenience alias so an Agent can keep all bake operations under one namespace.
 */
class FMonolithBakeActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleBakeZoneGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBakeNavMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBakeLighting(const TSharedPtr<FJsonObject>& Params);
};
