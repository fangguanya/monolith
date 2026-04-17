// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * `console.*` MCP namespace — generic console command / CVar access.
 *
 * Tools (3):
 *   - console.execute  : run any console command against the Editor or PIE world
 *   - console.get_cvar : read a CVar's current value
 *   - console.set_cvar : set a CVar (respects scalability limits)
 *
 * Use the editor world for `showflag.*` etc., the PIE world for runtime debug like
 * `stat Mass` / `masstraffic.debug.ShowAll 1`.
 */
class FMonolithConsoleActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleExecute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetCVar(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetCVar(const TSharedPtr<FJsonObject>& Params);
};
