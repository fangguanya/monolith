#include "MonolithIndexRuntimeState.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

/*
 * runtime state 测试主要检查三件事：
 * - stale 语义是否正确；
 * - 分页游标是否稳定；
 * - 路径前缀匹配是否符合预期。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRuntimeStateStaleSemanticsTest,
	"Monolith.Index.RuntimeState.StaleSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRuntimeStateStaleSemanticsTest::RunTest(const FString& Parameters)
{
	// 一轮简单会话里，先标记两条 stale，再推进一格进度。
	FMonolithIndexRuntimeState State;

	TSet<FString> StalePackages;
	StalePackages.Add(TEXT("/Game/A"));
	StalePackages.Add(TEXT("/Game/B"));

	State.BeginSession(StalePackages, 4);
	State.UpdateProgress(1, 4);

	const FMonolithIndexRuntimeSnapshot Snapshot = State.Snapshot();
	TestTrue(TEXT("session should be active"), Snapshot.bIndexingInProgress);
	TestTrue(TEXT("queued package A should be stale"), State.IsPackageStale(TEXT("/Game/A")));
	TestFalse(TEXT("untracked package should not be stale"), State.IsPackageStale(TEXT("/Game/C")));
	TestEqual(TEXT("stale package count should match tracked set"), Snapshot.StalePackageCount, 2);
	TestEqual(TEXT("remaining work should use progress items"), Snapshot.RemainingItems, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRuntimeStatePaginationTest,
	"Monolith.Index.RuntimeState.StalePagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRuntimeStatePaginationTest::RunTest(const FString& Parameters)
{
	// 故意打乱插入顺序，确认分页前会被排序成稳定顺序。
	FMonolithIndexRuntimeState State;

	TSet<FString> StalePackages;
	StalePackages.Add(TEXT("/Game/C"));
	StalePackages.Add(TEXT("/Game/A"));
	StalePackages.Add(TEXT("/Game/B"));

	State.BeginSession(StalePackages, 3);

	const TSharedPtr<FJsonObject> FirstPage = State.BuildStalePackagesPage(2, FString());
	TestTrue(TEXT("first page should be valid"), FirstPage.IsValid());
	TestEqual(TEXT("first page count"), static_cast<int32>(FirstPage->GetNumberField(TEXT("count"))), 2);

	const TArray<TSharedPtr<FJsonValue>>* FirstPackages = nullptr;
	TestTrue(TEXT("first page packages present"), FirstPage->TryGetArrayField(TEXT("packages"), FirstPackages));
	TestEqual(TEXT("packages should be alphabetically sorted"), (*FirstPackages)[0]->AsString(), FString(TEXT("/Game/A")));

	const FString Cursor = FirstPage->GetStringField(TEXT("next_cursor"));
	const TSharedPtr<FJsonObject> SecondPage = State.BuildStalePackagesPage(2, Cursor);
	TestEqual(TEXT("second page count"), static_cast<int32>(SecondPage->GetNumberField(TEXT("count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* SecondPackages = nullptr;
	TestTrue(TEXT("second page packages present"), SecondPage->TryGetArrayField(TEXT("packages"), SecondPackages));
	TestEqual(TEXT("second page contains the last package"), (*SecondPackages)[0]->AsString(), FString(TEXT("/Game/C")));
	TestTrue(TEXT("final page cursor should be empty"), SecondPage->GetStringField(TEXT("next_cursor")).IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRuntimeStatePrefixMatchTest,
	"Monolith.Index.RuntimeState.PrefixMatching",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRuntimeStatePrefixMatchTest::RunTest(const FString& Parameters)
{
	// Normalize 会自动补前导斜杠和清理尾部斜杠。
	const TArray<FString> Prefixes = FMonolithIndexRuntimeState::NormalizePackagePrefixes(
		{ TEXT("/Game/"), TEXT("PluginA/"), TEXT("/Shared") });

	TestEqual(TEXT("normalized prefix count"), Prefixes.Num(), 3);
	TestTrue(TEXT("/Game package should match"), FMonolithIndexRuntimeState::PackageMatchesAnyPrefix(TEXT("/Game/Foo/Bar"), Prefixes));
	TestTrue(TEXT("plugin package should match"), FMonolithIndexRuntimeState::PackageMatchesAnyPrefix(TEXT("/PluginA/Content/Asset"), Prefixes));
	TestFalse(TEXT("similar prefix should not match"), FMonolithIndexRuntimeState::PackageMatchesAnyPrefix(TEXT("/GameX/Asset"), Prefixes));

	return true;
}
