#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Python execution actions for Monolith.
 * Wraps IPythonScriptPlugin so external MCP clients can run UE Editor Python
 * code with captured stdout/stderr and structured error reporting.
 */
class FMonolithPythonActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExecuteFile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Evaluate(const TSharedPtr<FJsonObject>& Params);
};
