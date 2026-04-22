#include "Commandlets/MonolithIndexCommandletSupport.h"
#include "Commandlets/MonolithWarmupHistory.h"

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

/*
 * 这组测试主要守住 warmup commandlet 的“协议层”行为：
 * - 命令行参数能不能正确解析；
 * - 时间窗口会不会按预期停下来；
 * - 哪些 commandlet 该绕过本地 SQLite；
 * - 历史记录和 release gate streak 算法是不是稳定。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletParseScopeTest,
	"Monolith.Index.WarmupCommandlet.ParseScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletParseScopeTest::RunTest(const FString& Parameters)
{
	// 这里故意把几种常见参数一起塞进来，检查解析后字段是否完整保留。
	FMonolithWarmupCommandletArgs Args;
	FString Error;
	const bool bParsed = ParseMonolithWarmupCommandletArgs(
		TEXT("-Scope=Cohort:GenericAsset -Priority=Background -TimeWindowMinutes=15 -MaxPackages=32"),
		Args,
		Error);

	TestTrue(TEXT("scope should parse"), bParsed);
	TestTrue(TEXT("parse error should stay empty"), Error.IsEmpty());
	TestEqual(TEXT("scope kind should be cohort"), Args.Scope.Kind, EMonolithWarmupScopeKind::Cohort);
	TestEqual(TEXT("cohort name should be preserved"), Args.Scope.CohortName, FName(TEXT("GenericAsset")));
	TestEqual(TEXT("priority should parse"), Args.Priority, FString(TEXT("Background")));
	TestEqual(TEXT("time window should parse"), Args.TimeWindowMinutes, 15);
	TestEqual(TEXT("max packages should parse"), Args.MaxPackages, 32);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletTimeWindowTest,
	"Monolith.Index.WarmupCommandlet.TimeWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletTimeWindowTest::RunTest(const FString& Parameters)
{
	// 1 分钟窗口里，59 秒不该停，61 秒应该停。
	FMonolithWarmupCommandletArgs Args;
	Args.TimeWindowMinutes = 1;

	TestFalse(TEXT("before the deadline work should continue"), Args.ShouldStopForTimeWindow(0.0, 59.0));
	TestTrue(TEXT("after the deadline commandlet should stop"), Args.ShouldStopForTimeWindow(0.0, 61.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletBypassSqliteTest,
	"Monolith.Index.WarmupCommandlet.BypassSqlite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletBypassSqliteTest::RunTest(const FString& Parameters)
{
	// 只有 Monolith 自己那两个特殊 commandlet 才需要绕过本地 SQLite。
	TestTrue(TEXT("warmup commandlet should bypass local sqlite"), ShouldMonolithCommandletBypassLocalSqlite(TEXT("MonolithIndexWarmup")));
	TestTrue(TEXT("identity poc commandlet should bypass local sqlite"), ShouldMonolithCommandletBypassLocalSqlite(TEXT("MonolithIdentityPoc")));
	TestFalse(TEXT("other commandlets should not bypass sqlite"), ShouldMonolithCommandletBypassLocalSqlite(TEXT("ResavePackages")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletScopeTargetsIndexerTest,
	"Monolith.Index.WarmupCommandlet.ScopeTargetsIndexer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletScopeTargetsIndexerTest::RunTest(const FString& Parameters)
{
	FMonolithWarmupScope DependencyScope;
	DependencyScope.Kind = EMonolithWarmupScopeKind::Cohort;
	DependencyScope.CohortName = FName(TEXT("Dependency"));

	TestTrue(
		TEXT("explicit dependency cohort should match the dependency companion indexer id"),
		DoesMonolithWarmupScopeTargetIndexer(
			DependencyScope,
			FName(TEXT("Dependency")),
			TEXT("DependencyIndexer"),
			EMonolithExecutionMode::AROnly));
	TestFalse(
		TEXT("explicit dependency cohort should not accidentally match blueprint"),
		DoesMonolithWarmupScopeTargetIndexer(
			DependencyScope,
			FName(TEXT("Blueprint")),
			TEXT("BlueprintIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));

	FMonolithWarmupScope MeshCatalogScope;
	MeshCatalogScope.Kind = EMonolithWarmupScopeKind::Cohort;
	MeshCatalogScope.CohortName = FName(TEXT("MeshCatalog"));
	TestTrue(
		TEXT("explicit mesh catalog cohort should match the mesh catalog companion indexer id"),
		DoesMonolithWarmupScopeTargetIndexer(
			MeshCatalogScope,
			FName(TEXT("MeshCatalog")),
			TEXT("MeshCatalogIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));
	TestFalse(
		TEXT("explicit mesh catalog cohort should not accidentally match generic asset"),
		DoesMonolithWarmupScopeTargetIndexer(
			MeshCatalogScope,
			FName(TEXT("GenericAsset")),
			TEXT("GenericAssetIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));

	FMonolithWarmupScope GASScope;
	GASScope.Kind = EMonolithWarmupScopeKind::Cohort;
	GASScope.CohortName = FName(TEXT("GAS"));
	TestTrue(
		TEXT("explicit GAS cohort should match the GAS companion indexer id"),
		DoesMonolithWarmupScopeTargetIndexer(
			GASScope,
			FName(TEXT("GAS")),
			TEXT("GASIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));
	TestFalse(
		TEXT("explicit GAS cohort should not accidentally match blueprint"),
		DoesMonolithWarmupScopeTargetIndexer(
			GASScope,
			FName(TEXT("Blueprint")),
			TEXT("BlueprintIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));

	FMonolithWarmupScope BehaviorTreeScope;
	BehaviorTreeScope.Kind = EMonolithWarmupScopeKind::Cohort;
	BehaviorTreeScope.CohortName = FName(TEXT("BehaviorTree"));
	TestTrue(
		TEXT("explicit behavior tree cohort should match the behavior tree indexer id"),
		DoesMonolithWarmupScopeTargetIndexer(
			BehaviorTreeScope,
			FName(TEXT("BehaviorTree")),
			TEXT("BehaviorTreeIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));
	TestFalse(
		TEXT("explicit behavior tree cohort should not accidentally match eqs"),
		DoesMonolithWarmupScopeTargetIndexer(
			BehaviorTreeScope,
			FName(TEXT("EQS")),
			TEXT("EQSIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));

	FMonolithWarmupScope StateTreeScope;
	StateTreeScope.Kind = EMonolithWarmupScopeKind::Cohort;
	StateTreeScope.CohortName = FName(TEXT("StateTree"));
	TestTrue(
		TEXT("explicit state tree cohort should match the state tree indexer id"),
		DoesMonolithWarmupScopeTargetIndexer(
			StateTreeScope,
			FName(TEXT("StateTree")),
			TEXT("StateTreeIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));
	TestFalse(
		TEXT("explicit state tree cohort should not accidentally match behavior tree"),
		DoesMonolithWarmupScopeTargetIndexer(
			StateTreeScope,
			FName(TEXT("BehaviorTree")),
			TEXT("BehaviorTreeIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));

	FMonolithWarmupScope GlobalReducerScope;
	GlobalReducerScope.Kind = EMonolithWarmupScopeKind::GlobalReducer;
	TestTrue(
		TEXT("global reducer scope should only target reducer execution mode"),
		DoesMonolithWarmupScopeTargetIndexer(
			GlobalReducerScope,
			FName(TEXT("Reducer")),
			TEXT("ReducerIndexer"),
			EMonolithExecutionMode::GlobalReducer));
	TestFalse(
		TEXT("global reducer scope should ignore non-reducer execution modes"),
		DoesMonolithWarmupScopeTargetIndexer(
			GlobalReducerScope,
			FName(TEXT("Blueprint")),
			TEXT("BlueprintIndexer"),
			EMonolithExecutionMode::PackageScopedLoad));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletHitRateTest,
	"Monolith.Index.WarmupCommandlet.CacheHitRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletHitRateTest::RunTest(const FString& Parameters)
{
	// 命中率 = (本地命中 + 远端命中) / attempted packages。
	FMonolithArtifactCacheStats Stats;
	Stats.LocalHitCount = 7;
	Stats.RemoteHitCount = 2;
	Stats.RemoteMissCount = 1;

	TestTrue(
		TEXT("hit rate should use local + remote hits over attempted packages"),
		FMath::IsNearlyEqual(ComputeMonolithWarmupHitRatePercent(Stats, 10), 90.0));
	TestTrue(
		TEXT("zero attempted packages should report zero hit rate"),
		FMath::IsNearlyEqual(ComputeMonolithWarmupHitRatePercent(Stats, 0), 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletReleaseGateHistoryTest,
	"Monolith.Index.WarmupCommandlet.ReleaseGateHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWarmupCommandletReleaseThresholdClampTest,
	"Monolith.Index.WarmupCommandlet.ReleaseThresholdClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWarmupCommandletReleaseThresholdClampTest::RunTest(const FString& Parameters)
{
	// release gate 阈值的单位固定是 0-100 百分比整数，
	// 所以运行时也必须把越界输入钳回合法范围。
	TestEqual(TEXT("negative threshold should clamp to zero"), NormalizeMonolithWarmupReleaseThresholdPercent(-5), 0);
	TestEqual(TEXT("threshold above one hundred should clamp to one hundred"), NormalizeMonolithWarmupReleaseThresholdPercent(135), 100);
	TestEqual(TEXT("in-range threshold should stay unchanged"), NormalizeMonolithWarmupReleaseThresholdPercent(90), 90);
	return true;
}

bool FMonolithWarmupCommandletReleaseGateHistoryTest::RunTest(const FString& Parameters)
{
	// 用临时文件模拟历史，避免污染真实 warmup_history.json。
	const FString HistoryPath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithWarmupHistory"), TEXT(".json"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*HistoryPath, false, true);
	};

	FMonolithWarmupRunRecord FirstRun;
	// Blueprint 先来一条未达标记录。
	FirstRun.ScopeKey = TEXT("Cohort:Blueprint");
	FirstRun.ScopeDisplay = FirstRun.ScopeKey;
	FirstRun.StartedAtUtc = TEXT("2026-04-20T10:00:00Z");
	FirstRun.AttemptedPackages = 10;
	FirstRun.WarmedPackages = 10;
	FirstRun.CacheHitRatePercent = 88.0;

	FMonolithWarmupRunRecord OtherScopeRun = FirstRun;
	// 插一条其它 scope，验证它不会把 Blueprint 的 streak 打断。
	OtherScopeRun.ScopeKey = TEXT("Cohort:Material");
	OtherScopeRun.ScopeDisplay = OtherScopeRun.ScopeKey;
	OtherScopeRun.StartedAtUtc = TEXT("2026-04-20T10:05:00Z");
	OtherScopeRun.CacheHitRatePercent = 95.0;

	FMonolithWarmupRunRecord PassingRun = FirstRun;
	PassingRun.StartedAtUtc = TEXT("2026-04-20T10:10:00Z");
	PassingRun.CacheHitRatePercent = 92.0;

	FMonolithWarmupRunRecord PassingRunTwo = FirstRun;
	PassingRunTwo.StartedAtUtc = TEXT("2026-04-20T10:15:00Z");
	PassingRunTwo.CacheHitRatePercent = 96.0;

	TestTrue(TEXT("first history append should succeed"), AppendMonolithWarmupHistoryRecord(HistoryPath, FirstRun));
	TestTrue(TEXT("other scope append should succeed"), AppendMonolithWarmupHistoryRecord(HistoryPath, OtherScopeRun));
	TestTrue(TEXT("passing history append should succeed"), AppendMonolithWarmupHistoryRecord(HistoryPath, PassingRun));
	TestTrue(TEXT("second passing history append should succeed"), AppendMonolithWarmupHistoryRecord(HistoryPath, PassingRunTwo));

	TArray<FMonolithWarmupRunRecord> History;
	TestTrue(TEXT("history should load"), LoadMonolithWarmupHistory(HistoryPath, History));
	TestEqual(TEXT("history should contain all appended entries"), History.Num(), 4);
	TestEqual(
		TEXT("same-scope passing runs should count consecutively even with unrelated scopes in between"),
		CountConsecutiveThresholdPassingWarmupRuns(History, TEXT("Cohort:Blueprint"), 90.0),
		2);
	TestEqual(
		TEXT("higher threshold should reduce passing streak"),
		CountConsecutiveThresholdPassingWarmupRuns(History, TEXT("Cohort:Blueprint"), 95.0),
		1);
	return true;
}
