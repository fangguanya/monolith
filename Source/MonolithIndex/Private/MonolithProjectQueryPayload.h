#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithIndexDatabase.h"

/*
 * 这组 helper 专门负责把 MonolithIndex 的查询结果整理成 MCP action 返回的 JSON。
 *
 * 把它们单独收口有两个目的：
 * 1. `project.search` / `project.get_stats` 不再各自手搓一份 JSON 拼装逻辑；
 * 2. 纯 payload 语义可以直接做自动化测试，不必为了验证字段规则去驱动整个编辑器子系统。
 */
namespace MonolithProjectQueryPayload
{
	/** 把 search 结果、运行状态和常用统计字段整理成统一响应。 */
	TSharedPtr<FJsonObject> BuildSearchResponse(
		const TArray<FSearchResult>& SearchResults,
		bool bIndexingInProgress,
		float Progress,
		const TSharedPtr<FJsonObject>& Stats);

	/** 把 stats 对象提升成 MCP action 顶层响应，同时保留关键字段的扁平副本。 */
	TSharedPtr<FJsonObject> BuildStatsResponse(
		const TSharedPtr<FJsonObject>& Stats,
		bool bIndexing,
		float Progress,
		const FString& Status);
}
