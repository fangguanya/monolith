#include "Actions/ProjectListStalePackagesAction.h"

#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexRuntimeState.h"
#include "MonolithOfflineWarmupQueue.h"
#include "MonolithParamSchema.h"

/*
 * 这个 action 本身很薄，主要价值在于把分页参数规范化后转交给子系统。
 *
 * 真正的分页逻辑在 runtime state / subsystem 里，
 * 这里负责：
 * - 读参数；
 * - 拿子系统；
 * - 把结果包装成 MCP 风格 JSON。
 */

FMonolithActionResult FProjectListStalePackagesAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	// 默认每页 100 条，cursor 缺失则表示从第一页开始。
	const int32 Limit = MonolithProjectActionUtils::GetOptionalIntParam(Params, TEXT("limit"), 100);
	const FString Cursor = MonolithProjectActionUtils::GetOptionalStringParam(Params, TEXT("cursor"));

	TSet<FString> Packages;
	AppendMonolithOfflineWarmupQueuedPackages(Packages);

	const TSharedPtr<FJsonObject> Result = FMonolithIndexRuntimeState::BuildPackagePage(Packages, Limit, Cursor);
	if (!Result.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to list stale packages"));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("indexing_in_progress"), false);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectListStalePackagesAction::GetSchema()
{
	// cursor 是不透明字符串，调用方只需要原样回传即可。
	return FParamSchemaBuilder()
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum packages to return per page"), TEXT("100"))
		.Optional(TEXT("cursor"), TEXT("string"), TEXT("Opaque pagination cursor from a previous response"))
		.Build();
}
