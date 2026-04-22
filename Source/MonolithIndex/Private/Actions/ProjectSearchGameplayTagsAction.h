#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/*
 * 这个 action 用来“按关键字搜索 GameplayTag”。
 *
 * 和 list_gameplay_tags 的区别是：
 * - list 更像浏览目录；
 * - search 更像模糊搜索，并且会把引用这些 tag 的资产路径也带回来。
 */
class FProjectSearchGameplayTagsAction
{
public:
	/** 读取参数、执行查询并组装 MCP 返回值。 */
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	/** MCP 里暴露出去的动作名。 */
	static FString GetName() { return TEXT("search_gameplay_tags"); }
	/** 给工具列表看的简短说明。 */
	static FString GetDescription() { return TEXT("Search gameplay tags by substring and return matching tags with their referencing assets"); }
	/** 返回参数 schema。 */
	static TSharedPtr<FJsonObject> GetSchema();
};
