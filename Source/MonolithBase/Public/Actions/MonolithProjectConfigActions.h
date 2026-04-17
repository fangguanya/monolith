// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * `project.*` MCP namespace extension — read / write / reload Default*.ini.
 *
 * Tools (3):
 *   - project.read_config  : return value at file/section/key
 *   - project.write_config : set value (auto p4 edit-aware via FConfigCacheIni)
 *   - project.reload_config: invoke ReloadConfig on a UCLASS so DefaultConfig changes apply hot
 */
class FMonolithProjectConfigActions
{
public:
	static void RegisterActions();

private:
	static FMonolithActionResult HandleReadConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleWriteConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReloadConfig(const TSharedPtr<FJsonObject>& Params);
};
