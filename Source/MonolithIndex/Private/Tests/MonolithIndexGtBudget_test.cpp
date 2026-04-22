#include "Misc/AutomationTest.h"
#include "MonolithIndexGtBudget.h"

/*
 * GT budget 测试覆盖：
 * - 连续超预算会不会打开 breaker；
 * - downgrade 是否按 indexer 去重计数；
 * - quarantined indexer 和总 breaker 会不会触发 throttle。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexGtBudgetBreakerTest,
	"Monolith.Index.GtBudget.BreakerOpensAfterThreeOverruns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexGtBudgetBreakerTest::RunTest(const FString& Parameters)
{
	// 同一个 indexer 连续三次超 0.1s，就应当触发总 breaker。
	FMonolithIndexGtBudgetState State;
	State.RecordSample(FName(TEXT("Blueprint")), 0.11, 1.0);
	State.RecordSample(FName(TEXT("Blueprint")), 0.12, 2.0);
	State.RecordSample(FName(TEXT("Blueprint")), 0.13, 3.0);

	const FMonolithIndexGtBudgetSnapshot Snapshot = State.Snapshot(3.0);
	TestEqual(TEXT("three overruns should be counted"), Snapshot.OverrunCount, static_cast<uint64>(3));
	TestEqual(TEXT("same indexer should only be downgraded once"), Snapshot.DowngradeCount, static_cast<uint64>(1));
	TestTrue(TEXT("breaker should open after three consecutive overruns"), Snapshot.bBreakerOpen);
	TestTrue(TEXT("breaker should report remaining time"), Snapshot.BreakerRemainingSeconds > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexGtBudgetDowngradeTest,
	"Monolith.Index.GtBudget.DowngradeCountsPerIndexer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexGtBudgetDowngradeTest::RunTest(const FString& Parameters)
{
	// 不同 indexer 各自第一次超预算时都应该各记一次 downgrade。
	FMonolithIndexGtBudgetState State;
	State.RecordSample(FName(TEXT("Blueprint")), 0.20, 1.0);
	State.RecordSample(FName(TEXT("Blueprint")), 0.05, 2.0);
	State.RecordSample(FName(TEXT("Material")), 0.21, 3.0);
	State.RecordSample(FName(TEXT("Material")), 0.22, 4.0);

	const FMonolithIndexGtBudgetSnapshot Snapshot = State.Snapshot(4.0);
	TestEqual(TEXT("overrun counter should include all slow jobs"), Snapshot.OverrunCount, static_cast<uint64>(3));
	TestEqual(TEXT("downgrade counter should count each indexer once"), Snapshot.DowngradeCount, static_cast<uint64>(2));
	TestFalse(TEXT("breaker should stay closed when the overrun streak is interrupted"), Snapshot.bBreakerOpen);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexGtBudgetThrottleTest,
	"Monolith.Index.GtBudget.ThrottleAppliesToQuarantinedIndexer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexGtBudgetThrottleTest::RunTest(const FString& Parameters)
{
	// 被隔离的 indexer 立即避开 GT；总 breaker 打开后，所有 indexer 都应被节流。
	FMonolithIndexGtBudgetState State;
	State.RecordSample(FName(TEXT("GenericAsset")), 0.12, 1.0);

	TestTrue(TEXT("quarantined indexer should avoid GT load immediately"), State.ShouldAvoidGameThreadLoad(FName(TEXT("GenericAsset")), 1.5));
	TestFalse(TEXT("other indexers should not be throttled without breaker"), State.ShouldAvoidGameThreadLoad(FName(TEXT("Blueprint")), 1.5));

	State.RecordSample(FName(TEXT("Blueprint")), 0.13, 2.0);
	State.RecordSample(FName(TEXT("Blueprint")), 0.14, 3.0);
	TestTrue(TEXT("open breaker should throttle all indexers"), State.ShouldAvoidGameThreadLoad(FName(TEXT("Material")), 3.5));
	return true;
}
