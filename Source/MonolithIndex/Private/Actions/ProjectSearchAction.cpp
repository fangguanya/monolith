#include "Actions/ProjectSearchAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithProjectQueryPayload.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

/*
 * 这个 action 是最常用的“项目全文搜索”入口。
 *
 * 它会调用子系统的 Search，
 * 然后把结果整理成 MCP 友好的 JSON：
 * - 命中的资产列表；
 * - 当前是否正在索引；
 * - 进度、剩余数量、ETA 这些状态信息。
 */

FMonolithActionResult FProjectSearchAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!MonolithProjectActionUtils::TryGetRequiredStringParam(Params, TEXT("query"), Query))
	{
		return MonolithProjectActionUtils::MakeMissingStringParamError(TEXT("query"));
	}

	const int32 Limit = MonolithProjectActionUtils::GetOptionalIntParam(Params, TEXT("limit"), 50);

	return MonolithProjectActionUtils::RunReadDatabaseAction(
		[&](FMonolithIndexDatabase& Database) -> FMonolithActionResult
		{
			const TArray<FSearchResult> SearchResults = Database.FullTextSearch(Query, Limit);
			const TSharedPtr<FJsonObject> Stats = Database.GetStats();
			return FMonolithActionResult::Success(
				MonolithProjectQueryPayload::BuildSearchResponse(
					SearchResults,
					false,
					0.0f,
					Stats));
		});
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS5 search query (supports AND, OR, NOT, prefix*)"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return"), TEXT("50"))
		.Build();
}
