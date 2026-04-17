// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "MonolithMassInternal.h"

/**
 * `masstraffic.*` MCP namespace — CitySample Traffic plugin operations.
 *
 * Tools (5):
 *   - list_field_actors           : AMassTrafficFieldActor list
 *   - get_field_config            : single field actor's bEnabled / LaneTagFilter / Extent
 *   - set_field_config            : toggle bEnabled, set LaneTagFilter.AnyTags by tag name
 *   - rebuild_zone_graph_modifier : invoke AMassTrafficZoneGraphDataModifier::BuildZoneGraphData
 *                                   on the first modifier found in the world (via reflection, to
 *                                   avoid a hard compile-time dep on MassTrafficEditor).
 *   - get_settings                : UMassTrafficSettings traffic / crosswalk lane filter summary
 */
class FMonolithMassTrafficActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_MASSTRAFFIC
private:
	static FMonolithActionResult HandleListFieldActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetFieldConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetFieldConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRebuildZoneGraphModifier(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSettings(const TSharedPtr<FJsonObject>& Params);
#endif
};
