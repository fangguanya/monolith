#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MonolithProjectQueryPayload.h"

/*
 * 这组测试只守住“查询 payload 怎么拼”这层纯逻辑语义：
 * - search 顶层 stale 是否正确聚合；
 * - stats 是否把 indexing 与 stale_packages 分开暴露。
 *
 * 这里故意不去驱动整个 editor subsystem，
 * 因为我们要验证的是返回协议本身，而不是编辑器生命周期。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectQueryPayloadSearchTest,
	"Monolith.Index.QueryPayload.SearchAggregatesStaleAndProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectQueryPayloadSearchTest::RunTest(const FString& Parameters)
{
	TArray<FSearchResult> SearchResults;

	FSearchResult FreshResult;
	FreshResult.AssetPath = TEXT("/Game/Fresh");
	FreshResult.AssetName = TEXT("Fresh");
	FreshResult.AssetClass = TEXT("Blueprint");
	FreshResult.ModuleName = TEXT("Game");
	FreshResult.MatchContext = TEXT("asset_name");
	FreshResult.Rank = 1.0f;
	FreshResult.bStale = false;
	SearchResults.Add(FreshResult);

	FSearchResult StaleResult;
	StaleResult.AssetPath = TEXT("/Game/Stale");
	StaleResult.AssetName = TEXT("Stale");
	StaleResult.AssetClass = TEXT("Material");
	StaleResult.ModuleName = TEXT("Game");
	StaleResult.MatchContext = TEXT("description");
	StaleResult.Rank = 0.5f;
	StaleResult.bStale = true;
	SearchResults.Add(StaleResult);

	TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("remaining_items"), 7);
	Stats->SetNumberField(TEXT("eta_seconds"), 12.5);

	const TSharedPtr<FJsonObject> Response = MonolithProjectQueryPayload::BuildSearchResponse(
		SearchResults,
		true,
		0.25f,
		Stats);
	TestTrue(TEXT("search payload should be created"), Response.IsValid());
	TestTrue(TEXT("search payload should report success"), Response->GetBoolField(TEXT("success")));
	TestTrue(TEXT("search payload should preserve indexing flag"), Response->GetBoolField(TEXT("indexing_in_progress")));
	TestTrue(TEXT("search payload should aggregate top-level stale when any hit is stale"), Response->GetBoolField(TEXT("stale")));
	TestEqual(TEXT("search payload should expose result count"), static_cast<int32>(Response->GetNumberField(TEXT("count"))), 2);
	TestEqual(TEXT("search payload should copy remaining_items from stats"), static_cast<int32>(Response->GetNumberField(TEXT("remaining_items"))), 7);
	TestEqual(TEXT("search payload should copy eta_seconds from stats"), Response->GetNumberField(TEXT("eta_seconds")), 12.5);

	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	TestTrue(TEXT("search payload should include result array"), Response->TryGetArrayField(TEXT("results"), ResultsArray));
	TestEqual(TEXT("first hit should stay fresh"), (*ResultsArray)[0]->AsObject()->GetBoolField(TEXT("stale")), false);
	TestEqual(TEXT("second hit should stay stale"), (*ResultsArray)[1]->AsObject()->GetBoolField(TEXT("stale")), true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectQueryPayloadStatsTest,
	"Monolith.Index.QueryPayload.StatsKeepsIndexingSeparateFromStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectQueryPayloadStatsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetBoolField(TEXT("indexing_in_progress"), false);
	Stats->SetBoolField(TEXT("remote_disabled"), true);
	Stats->SetNumberField(TEXT("stale_packages"), 3);
	Stats->SetNumberField(TEXT("queue_depth"), 9);
	Stats->SetNumberField(TEXT("remaining_items"), 4);
	Stats->SetNumberField(TEXT("eta_seconds"), 18.0);
	Stats->SetStringField(TEXT("status"), TEXT("warming"));

	const TSharedPtr<FJsonObject> Response = MonolithProjectQueryPayload::BuildStatsResponse(
		Stats,
		true,
		0.75f,
		TEXT("indexing"));
	TestTrue(TEXT("stats payload should be created"), Response.IsValid());
	TestTrue(TEXT("stats payload should report success"), Response->GetBoolField(TEXT("success")));
	TestTrue(TEXT("top-level indexing should preserve subsystem running state"), Response->GetBoolField(TEXT("indexing")));
	TestFalse(TEXT("indexing_in_progress should still reflect stats payload exactly"), Response->GetBoolField(TEXT("indexing_in_progress")));
	TestTrue(TEXT("remote_disabled should be copied to top level"), Response->GetBoolField(TEXT("remote_disabled")));
	TestEqual(TEXT("stale package count should stay separate from indexing"), static_cast<int32>(Response->GetNumberField(TEXT("stale_packages"))), 3);
	TestEqual(TEXT("queue depth should be copied"), static_cast<int32>(Response->GetNumberField(TEXT("queue_depth"))), 9);
	TestEqual(TEXT("remaining items should be copied"), static_cast<int32>(Response->GetNumberField(TEXT("remaining_items"))), 4);
	TestEqual(TEXT("eta should be copied"), Response->GetNumberField(TEXT("eta_seconds")), 18.0);

	const TSharedPtr<FJsonObject>* NestedStats = nullptr;
	TestTrue(TEXT("stats payload should retain the full nested stats object"), Response->TryGetObjectField(TEXT("stats"), NestedStats));
	TestEqual(TEXT("nested stats should preserve original status"), (*NestedStats)->GetStringField(TEXT("status")), FString(TEXT("warming")));
	return true;
}
