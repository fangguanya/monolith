#include "MonolithProjectQueryPayload.h"

#include "Serialization/JsonTypes.h"

namespace MonolithProjectQueryPayload
{
	namespace
	{
		/** 如果 stats 里存在目标字段，就把它抄到顶层，避免 action 到处复制样板判断。 */
		template <typename CopyFn>
		void CopyFieldIfPresent(
			const TSharedPtr<FJsonObject>& Source,
			const TSharedPtr<FJsonObject>& Destination,
			const TCHAR* FieldName,
			const EJson JsonType,
			CopyFn&& Copy)
		{
			if (Source.IsValid() && Destination.IsValid() && Source->HasTypedField(FieldName, JsonType))
			{
				Copy();
			}
		}
	}

	TSharedPtr<FJsonObject> BuildSearchResponse(
		const TArray<FSearchResult>& SearchResults,
		const bool bIndexingInProgress,
		const float Progress,
		const TSharedPtr<FJsonObject>& Stats)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		bool bAnyStale = false;

		for (const FSearchResult& SearchResult : SearchResults)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), SearchResult.AssetPath);
			Entry->SetStringField(TEXT("asset_name"), SearchResult.AssetName);
			Entry->SetStringField(TEXT("asset_class"), SearchResult.AssetClass);
			Entry->SetStringField(TEXT("module_name"), SearchResult.ModuleName);
			Entry->SetStringField(TEXT("match_context"), SearchResult.MatchContext);
			Entry->SetNumberField(TEXT("rank"), SearchResult.Rank);
			Entry->SetBoolField(TEXT("stale"), SearchResult.bStale);
			bAnyStale |= SearchResult.bStale;
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("results"), ResultsArray);
		Result->SetNumberField(TEXT("count"), SearchResults.Num());
		Result->SetBoolField(TEXT("indexing_in_progress"), bIndexingInProgress);
		Result->SetBoolField(TEXT("stale"), bAnyStale);
		Result->SetNumberField(TEXT("progress"), Progress);

		// search 顶层只提升调用方最常用的两个进度字段，
		// 其余详细统计仍然统一放在 `project.get_stats`。
		CopyFieldIfPresent(Stats, Result, TEXT("remaining_items"), EJson::Number, [&]()
		{
			Result->SetNumberField(TEXT("remaining_items"), Stats->GetNumberField(TEXT("remaining_items")));
		});
		CopyFieldIfPresent(Stats, Result, TEXT("eta_seconds"), EJson::Number, [&]()
		{
			Result->SetNumberField(TEXT("eta_seconds"), Stats->GetNumberField(TEXT("eta_seconds")));
		});

		return Result;
	}

	TSharedPtr<FJsonObject> BuildStatsResponse(
		const TSharedPtr<FJsonObject>& Stats,
		const bool bIndexing,
		const float Progress,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("stats"), Stats);
		Result->SetBoolField(TEXT("indexing"), bIndexing);
		Result->SetNumberField(TEXT("progress"), Progress);
		Result->SetStringField(TEXT("status"), Status);

		// stats action 会把最常用字段平铺到顶层，
		// 这样外部脚本既能拿到完整 stats，也能少写一层 JSON 访问路径。
		CopyFieldIfPresent(Stats, Result, TEXT("indexing_in_progress"), EJson::Boolean, [&]()
		{
			Result->SetBoolField(TEXT("indexing_in_progress"), Stats->GetBoolField(TEXT("indexing_in_progress")));
		});
		CopyFieldIfPresent(Stats, Result, TEXT("remote_disabled"), EJson::Boolean, [&]()
		{
			Result->SetBoolField(TEXT("remote_disabled"), Stats->GetBoolField(TEXT("remote_disabled")));
		});
		CopyFieldIfPresent(Stats, Result, TEXT("gt_breaker_open"), EJson::Boolean, [&]()
		{
			Result->SetBoolField(TEXT("gt_breaker_open"), Stats->GetBoolField(TEXT("gt_breaker_open")));
		});

		for (const TCHAR* FieldName : {
			TEXT("progress"),
			TEXT("stale_packages"),
			TEXT("completed_items"),
			TEXT("total_items"),
			TEXT("queue_depth"),
			TEXT("remaining_items"),
			TEXT("eta_seconds"),
			TEXT("local_hit"),
			TEXT("remote_hit"),
			TEXT("remote_miss"),
			TEXT("remote_write_ok"),
			TEXT("remote_write_fail"),
			TEXT("remote_write_bytes"),
			TEXT("remote_write_mb"),
			TEXT("oversized_artifact"),
			TEXT("offline_queue_depth"),
			TEXT("remote_breaker_remaining_seconds"),
			TEXT("gt_overrun_count"),
			TEXT("gt_downgrade_count"),
			TEXT("gt_breaker_remaining_seconds")
		})
		{
			CopyFieldIfPresent(Stats, Result, FieldName, EJson::Number, [&, FieldName]()
			{
				Result->SetNumberField(FieldName, Stats->GetNumberField(FieldName));
			});
		}

		CopyFieldIfPresent(Stats, Result, TEXT("status"), EJson::String, [&]()
		{
			Result->SetStringField(TEXT("status"), Stats->GetStringField(TEXT("status")));
		});

		return Result;
	}
}
