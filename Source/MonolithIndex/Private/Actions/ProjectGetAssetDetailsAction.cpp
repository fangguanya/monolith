#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonTypes.h"

/*
 * 这个 action 提供“看单个资产详情”的 MCP 入口。
 *
 * 输入是一个包路径，
 * 输出是数据库里已经索引好的详情快照，再顺手补一个 stale 标记。
 */

FMonolithActionResult FProjectGetAssetDetailsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	// 这里统一只接受 package_path。
	// Monolith 的索引主键和 plan 里的对外语义都是“包路径”，不再保留模糊别名。
	FString PackagePath;
	if (!MonolithProjectActionUtils::TryGetRequiredStringParam(Params, TEXT("package_path"), PackagePath))
	{
		return MonolithProjectActionUtils::MakeMissingStringParamError(TEXT("package_path"));
	}

	UMonolithIndexSubsystem* const Subsystem = MonolithProjectActionUtils::GetIndexSubsystem();
	if (!Subsystem)
	{
		return MonolithProjectActionUtils::MakeSubsystemUnavailableError();
	}

	const TSharedPtr<FJsonObject> Details = Subsystem->GetAssetDetails(PackagePath);
	if (!Details.IsValid() || !Details->HasField(TEXT("asset_name")))
	{
		return FMonolithActionResult::Error(TEXT("Asset not found in index"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("asset"), Details);
	// stale 字段不是每条详情都一定有，所以这里做一次“有就拿，没有就 false”。
	Result->SetBoolField(TEXT("stale"), Details->HasTypedField<EJson::Boolean>(TEXT("stale")) ? Details->GetBoolField(TEXT("stale")) : false);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectGetAssetDetailsAction::GetSchema()
{
	// schema 主要给外部工具看，告诉它这个 action 需要什么参数。
	return FParamSchemaBuilder()
		.Required(TEXT("package_path"), TEXT("string"), TEXT("Package path of the asset (e.g. /Game/Characters/BP_Hero)"))
		.Build();
}
