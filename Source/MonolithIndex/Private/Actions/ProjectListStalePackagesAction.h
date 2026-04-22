#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/*
 * 这个 action 给外部工具提供“分页查看 stale 包列表”的入口。
 *
 * 因为 stale 包可能很多，所以这里不是一次全吐完，
 * 而是使用 limit + opaque cursor 的翻页方式。
 */
class FProjectListStalePackagesAction
{
public:
	/** 执行分页查询。 */
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	/** MCP action 名。 */
	static FString GetName() { return TEXT("list_stale_packages"); }
	/** 一句话说明。 */
	static FString GetDescription() { return TEXT("List stale project packages with opaque cursor pagination"); }
	/** 提供给外部工具的参数 schema。 */
	static TSharedPtr<FJsonObject> GetSchema();
};
