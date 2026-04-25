#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/MonolithProjectActionUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonTypes.h"

FMonolithActionResult FProjectFindReferencesAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	// 统一使用 package_path，避免 action 层继续扩散“asset_path / package_path”双名字分叉。
	FString PackagePath;
	if (!MonolithProjectActionUtils::TryGetRequiredStringParam(Params, TEXT("package_path"), PackagePath))
	{
		return MonolithProjectActionUtils::MakeMissingStringParamError(TEXT("package_path"));
	}

	return MonolithProjectActionUtils::RunReadDatabaseAction(
		[&](FMonolithIndexDatabase& Database) -> FMonolithActionResult
		{
			const TSharedPtr<FJsonObject> Refs = Database.FindReferences(PackagePath);
			if (!Refs.IsValid())
			{
				return FMonolithActionResult::Error(TEXT("Asset not found in index"));
			}

			auto Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("package_path"), PackagePath);
			Result->SetObjectField(TEXT("references"), Refs);
			return FMonolithActionResult::Success(Result);
		});
}

TSharedPtr<FJsonObject> FProjectFindReferencesAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("package_path"), TEXT("string"), TEXT("Package path of the asset (e.g. /Game/Characters/BP_Hero)"))
		.Build();
}
