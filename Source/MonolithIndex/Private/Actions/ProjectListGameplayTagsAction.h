#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/*
 * 这个 action 用来“列出当前索引里有哪些 GameplayTag”。
 *
 * 它支持一个可选 prefix 过滤，
 * 适合拿来做浏览、补全或快速查看某个命名空间下的 tag。
 */
class FProjectListGameplayTagsAction
{
public:
	/** 读取参数、执行查询并组装 MCP 返回值。 */
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	/** MCP 里暴露出去的动作名。 */
	static FString GetName() { return TEXT("list_gameplay_tags"); }
	/** 给工具列表看的简短说明。 */
	static FString GetDescription() { return TEXT("List all indexed gameplay tags, optionally filtered by prefix (e.g. 'Weapon.Melee')"); }
	/** 返回参数 schema。 */
	static TSharedPtr<FJsonObject> GetSchema();
};
