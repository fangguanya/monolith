// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Extends the `base.*` MCP namespace with UFUNCTION invocation on any AActor / UObject.
 *
 * Motivation:
 *   `base.set_actor_property` doesn't fire PostEditChangeProperty and cannot trigger a
 *   `CallInEditor` UFUNCTION. This class plugs that gap so scripted flows (Baker.BakeAll,
 *   SpawnerSetup.SpawnAllMassActors, etc.) can be driven purely from MCP.
 *
 * Tools registered (2):
 *   - base.invoke_function          — Invoke any UFUNCTION on a target Actor by label,
 *                                      with JSON-serialised input parameters.
 *   - base.call_in_editor_function  — Same, restricted to functions marked `CallInEditor=true`
 *                                      (safer for automation scripts).
 */
class FMonolithBaseFunctionActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleInvokeFunction(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCallInEditorFunction(const TSharedPtr<FJsonObject>& Params);
};
