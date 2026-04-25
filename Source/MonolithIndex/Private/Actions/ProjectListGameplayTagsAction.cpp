#include "Actions/ProjectListGameplayTagsAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"

/*
 * 这个 action 负责列出 GameplayTag 清单。
 *
 * 以前它会直接抓 raw SQLite 句柄自己查，
 * 现在改成走子系统/数据库正式接口，这样查询就能复用统一的数据库锁，
 * 避免后台索引和前台查询同时碰库时互相踩到。
 */

FMonolithActionResult FProjectListGameplayTagsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	const FString Prefix = MonolithProjectActionUtils::GetOptionalStringParam(Params, TEXT("prefix"));

	return MonolithProjectActionUtils::RunReadDatabaseAction(
		[&](FMonolithIndexDatabase& Database) -> FMonolithActionResult
		{
			const TArray<FIndexedGameplayTagSummary> TagSummaries = Database.ListGameplayTags(Prefix);
			TArray<TSharedPtr<FJsonValue>> TagsArr;
			for (const FIndexedGameplayTagSummary& TagSummary : TagSummaries)
			{
				auto Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("tag_name"), TagSummary.TagName);
				Entry->SetStringField(TEXT("parent_tag"), TagSummary.ParentTag);
				Entry->SetNumberField(TEXT("reference_count"), static_cast<double>(TagSummary.ReferenceCount));
				TagsArr.Add(MakeShared<FJsonValueObject>(Entry));
			}

			auto Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetArrayField(TEXT("tags"), TagsArr);
			Result->SetNumberField(TEXT("count"), TagsArr.Num());
			if (!Prefix.IsEmpty())
			{
				Result->SetStringField(TEXT("prefix_filter"), Prefix);
			}
			return FMonolithActionResult::Success(Result);
		});
}

TSharedPtr<FJsonObject> FProjectListGameplayTagsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("prefix"), TEXT("string"), TEXT("Tag prefix filter (e.g. 'Weapon.Melee') -- returns tags starting with this prefix"))
		.Build();
}
