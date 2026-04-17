#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintGraphExportActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Phase 5C — Graph export/import/copy
	static FMonolithActionResult HandleExportGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCopyNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateGraph(const TSharedPtr<FJsonObject>& Params);

	// T3D export — programmatic Ctrl+A+Ctrl+C for an arbitrary Blueprint graph.
	// Returns raw T3D text including full node state (FAnimNode_ struct fields, property bindings, etc.).
	static FMonolithActionResult HandleExportNodesT3D(const TSharedPtr<FJsonObject>& Params);
};
