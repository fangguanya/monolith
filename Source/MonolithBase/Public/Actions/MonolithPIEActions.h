// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * `pie.*` MCP namespace — Play-In-Editor lifecycle control.
 *
 * Tools (6):
 *   - pie.start              : kick off PIE in the current Editor viewport
 *   - pie.stop               : end the PIE session
 *   - pie.is_active          : return current PIE state
 *   - pie.pause              : pause the PIE world
 *   - pie.resume             : resume
 *   - pie.set_time_dilation  : fast-forward or slow-motion for deterministic waits
 */
class FMonolithPIEActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleStart(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStop(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleIsActive(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandlePause(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleResume(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetTimeDilation(const TSharedPtr<FJsonObject>& Params);
};
