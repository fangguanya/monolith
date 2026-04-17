#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * ABP-specific T3D dump actions.
 *
 * Monolith's generic blueprint.export_nodes_t3d works on BP->FunctionGraphs / UbergraphPages,
 * but ABP state machine inner graphs (states, transitions, conduits) and nested state machines
 * are not reachable from those top-level arrays. This class walks the full ABP graph tree and
 * produces a multi-graph T3D dump — the structural equivalent of Ctrl+A+Ctrl+C in every
 * subgraph editor.
 */
class FMonolithAbpDumpActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// dump_abp_t3d — recursive full-ABP T3D dump (all graphs including state machine inner graphs).
	static FMonolithActionResult HandleDumpAbpT3D(const TSharedPtr<FJsonObject>& Params);

	// list_abp_graphs — enumerate every graph reachable from an ABP (flat list with path).
	static FMonolithActionResult HandleListAbpGraphs(const TSharedPtr<FJsonObject>& Params);
};
