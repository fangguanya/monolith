// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "MonolithMassInternal.h"

/**
 * `zg.*` MCP namespace — ZoneGraph authoring, query, and self-test surface.
 *
 * All handlers are world-scoped and resolve the target world via MonolithMass::GetMcpTargetWorld,
 * preferring PIE when available.
 *
 * Original 7 tools (authoring + query):
 *   - list_zone_graph_data     : enumerate AZoneGraphData actors
 *   - list_zone_shapes         : enumerate AZoneShape actors + their tags
 *   - get_zone_shape_config    : points/tags/lane profile of a single AZoneShape
 *   - list_lane_tags           : names/indices from UZoneGraphSettings
 *   - build_zone_graph         : FZoneGraphBuilder::BuildAll on all ZoneGraphData in world
 *   - query_lanes_by_tag       : FZoneGraphStorage lane list filtered by tag name
 *   - get_storage_summary      : per-data totals (zones / lanes / points)
 *
 * CL-B.7 self-test tools (5 new — for PCT Baker verification):
 *   - get_lane_info            : width / length / start-end points / tag mask for one explicit
 *                                (data_index, lane_index). Handles the pair correctly; replaces
 *                                the bugged ai.get_zone_lane_info which always returns lane 0.
 *   - get_lane_links           : NextLanes / PrevLanes / AdjacentLanes for a lane, each with
 *                                destination handle + EZoneLaneLinkType + EZoneLaneLinkFlags.
 *                                Consumes FZoneGraphStorage::LaneLinks. Drives TC-3/4/5.
 *   - detect_lane_islands      : Flood-fill on lane connectivity; returns per-island lane count
 *                                and a sample handle. TC-6 asserts island_count == 1 for a fully
 *                                connected bake. Optional `tag_filter_any` restricts to matching lanes.
 *   - trace_lane_path          : FZoneGraphAStar::FindPath from start to end lane with optional
 *                                AnyTags / NotTags filters. Returns lane handle sequence.
 *                                TC-9 validates turn-restriction enforcement.
 *   - list_polygon_shape_points: Dump Shape Points of a Polygon AZoneShape: Position, Type
 *                                (Sharp/Bezier/LaneProfile/AutoLaneProfile), per-point LaneProfile
 *                                index. Verifies CL-B.5 polygon gates authored correctly.
 */
class FMonolithZoneGraphActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_ZONEGRAPH
private:
	// Original 7 tools — see header comment.
	static FMonolithActionResult HandleListZoneGraphData(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListZoneShapes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetZoneShapeConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListLaneTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildZoneGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleQueryLanesByTag(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStorageSummary(const TSharedPtr<FJsonObject>& Params);

	// CL-B.7 self-test MCP additions (5 new tools) — see class-level header comment above.
	static FMonolithActionResult HandleGetLaneInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLaneLinks(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDetectLaneIslands(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceLanePath(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListPolygonShapePoints(const TSharedPtr<FJsonObject>& Params);
#endif
};
