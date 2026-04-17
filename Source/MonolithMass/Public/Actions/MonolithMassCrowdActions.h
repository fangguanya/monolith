// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "MonolithMassInternal.h"

/**
 * `masscrowd.*` MCP namespace — Engine MassCrowd plugin operations.
 *
 * Tools (4):
 *   - list_spawners      : AMassSpawner actors tagged or named "Crowd" in the current world
 *   - get_settings       : UMassCrowdSettings CrowdTag / CrossingTag / lane densities
 *   - get_lane_density   : return FMassCrowdLaneDensityDesc list
 *   - rebuild_lane_data  : UMassCrowdSubsystem::RebuildLaneData (editor only)
 */
class FMonolithMassCrowdActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_MASSCROWD
private:
	static FMonolithActionResult HandleListSpawners(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLaneDensity(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRebuildLaneData(const TSharedPtr<FJsonObject>& Params);
#endif
};
