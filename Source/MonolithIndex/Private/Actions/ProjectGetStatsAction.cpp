#include "Actions/ProjectGetStatsAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithProjectQueryPayload.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

/*
 * 这个 action 是“项目索引仪表盘”的 MCP 入口。
 *
 * 它不只把数据库里的 stats 原样吐出去，
 * 还会顺手把运行时状态里的一些常用字段抄到顶层，
 * 这样调用方可以少写一层 JSON 解析。
 */

FMonolithActionResult FProjectGetStatsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;

	UMonolithIndexSubsystem* const Subsystem = MonolithProjectActionUtils::GetIndexSubsystem();
	if (!Subsystem)
	{
		return MonolithProjectActionUtils::MakeSubsystemUnavailableError();
	}

	const TSharedPtr<FJsonObject> Stats = Subsystem->GetStats();
	if (!Stats.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to retrieve stats"));
	}
	return FMonolithActionResult::Success(
		MonolithProjectQueryPayload::BuildStatsResponse(
			Stats,
			Subsystem->IsIndexing(),
			Subsystem->GetProgress(),
			Subsystem->GetStatusMessage()));
}

TSharedPtr<FJsonObject> FProjectGetStatsAction::GetSchema()
{
	// 这个 action 不需要额外参数，所以 schema 直接是空对象。
	return MakeShared<FJsonObject>();
}
