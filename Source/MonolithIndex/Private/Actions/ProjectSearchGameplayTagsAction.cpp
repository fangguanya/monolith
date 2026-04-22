#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"

/*
 * 这个 action 负责做 GameplayTag 模糊搜索。
 *
 * 返回里除了 tag 名本身，还会带上“哪些资产正在引用它”，
 * 这样外部工具在展示 tag 搜索结果时，可以直接给出上下文。
 */

FMonolithActionResult FProjectSearchGameplayTagsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!MonolithProjectActionUtils::TryGetRequiredStringParam(Params, TEXT("query"), Query))
	{
		return MonolithProjectActionUtils::MakeMissingStringParamError(TEXT("query"));
	}

	UMonolithIndexSubsystem* const Subsystem = MonolithProjectActionUtils::GetIndexSubsystem();
	if (!Subsystem)
	{
		return MonolithProjectActionUtils::MakeSubsystemUnavailableError();
	}

	const TArray<FIndexedGameplayTagSummary> TagSummaries = Subsystem->SearchGameplayTags(Query);
	TArray<TSharedPtr<FJsonValue>> TagsArr;
	for (const FIndexedGameplayTagSummary& TagSummary : TagSummaries)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag_name"), TagSummary.TagName);
		Entry->SetStringField(TEXT("parent_tag"), TagSummary.ParentTag);
		Entry->SetNumberField(TEXT("reference_count"), static_cast<double>(TagSummary.ReferenceCount));

		TArray<TSharedPtr<FJsonValue>> AssetsArr;
		for (const FString& AssetPath : TagSummary.ReferencingAssets)
		{
			AssetsArr.Add(MakeShared<FJsonValueString>(AssetPath));
		}
		Entry->SetArrayField(TEXT("referencing_assets"), AssetsArr);

		TagsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("tags"), TagsArr);
	Result->SetNumberField(TEXT("count"), TagsArr.Num());
	Result->SetStringField(TEXT("query"), Query);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchGameplayTagsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("Substring to search for in tag names (e.g. 'Damage', 'Weapon')"))
		.Build();
}
