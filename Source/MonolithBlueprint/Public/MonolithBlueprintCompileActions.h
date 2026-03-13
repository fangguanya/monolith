#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintCompileActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDependencies(const TSharedPtr<FJsonObject>& Params);
};
