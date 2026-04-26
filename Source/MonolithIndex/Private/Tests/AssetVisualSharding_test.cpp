#include "AssetVisualSharding.h"

#include "Misc/AutomationTest.h"

/*
 * AssetVisualSharding 测试覆盖：
 *  - shard key 计算 deterministic（同输入同输出）
 *  - 容量未超时不再拆
 *  - 容量超出时按下一段路径自动再拆
 *  - 同一资产在两次独立调用中得到的 shard key 完全一致（确保 Horde agent 和编辑器结果对齐）
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualShardKeyDeterministicTest,
	"Monolith.Index.AssetVisual.Sharding.KeyDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualShardKeyDeterministicTest::RunTest(const FString& Parameters)
{
	const FAssetVisualShardKey K1 = ComputeAssetVisualShardKey(TEXT("/Game/Buildings/Houses/SM_House.SM_House"), 2);
	const FAssetVisualShardKey K2 = ComputeAssetVisualShardKey(TEXT("/Game/Buildings/Houses/SM_House.SM_House"), 2);
	TestEqual(TEXT("same input must yield same shard id"), K1.ShardId, K2.ShardId);
	TestEqual(TEXT("prefix depth preserved"), K1.PrefixDepth, 2);
	TestEqual(TEXT("expected shard id at depth 2"), K1.ShardId, FString(TEXT("Game.Buildings")));

	const FAssetVisualShardKey K3 = ComputeAssetVisualShardKey(TEXT("/Game/Buildings/Houses/SM_House.SM_House"), 3);
	TestEqual(TEXT("expected shard id at depth 3"), K3.ShardId, FString(TEXT("Game.Buildings.Houses")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualShardAutoSplitTest,
	"Monolith.Index.AssetVisual.Sharding.AutoSplitWhenOverCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualShardAutoSplitTest::RunTest(const FString& Parameters)
{
	// 构造一个能被路径段切分递归拆开的分布：每 5 个 mesh 落进一个独立子目录
	// `/Game/Vehicles/Cars/Cluster_NN/SM_Car_*`，路径深度足以让算法把整堆裁到容量内。
	TArray<FString> Paths;
	const int32 ClusterCount = 20;     // 20 个 cluster 子目录
	const int32 PerCluster = 5;        // 每个 5 mesh
	for (int32 Cluster = 0; Cluster < ClusterCount; ++Cluster)
	{
		for (int32 InCluster = 0; InCluster < PerCluster; ++InCluster)
		{
			const int32 Index = Cluster * PerCluster + InCluster;
			Paths.Add(FString::Printf(
				TEXT("/Game/Vehicles/Cars/Cluster_%02d/SM_Car_%03d.SM_Car_%03d"),
				Cluster, Index, Index));
		}
	}

	FAssetVisualShardCapacityPolicy Policy;
	Policy.InitialPrefixDepth = 2;            // 起步 /Game/Vehicles
	Policy.MaxMeshesPerShard = 10;            // 上限 10：每个 cluster (5 mesh) 落得下，但 Cars 整层 (100 mesh) 不行
	Policy.MaxPrefixDepth = 6;

	const TMap<FString, FAssetVisualShardKey> Assignment = AssignAssetVisualShardsForCohort(Paths, Policy);
	TestEqual(TEXT("all paths must be assigned"), Assignment.Num(), Paths.Num());

	// 统计最终 shard 分布。
	TMap<FString, int32> CountByShard;
	for (const TPair<FString, FAssetVisualShardKey>& Pair : Assignment)
	{
		++CountByShard.FindOrAdd(Pair.Value.ShardId);
	}

	// 算法可拆得开时，所有 shard 必须落在容量内。
	for (const TPair<FString, int32>& Pair : CountByShard)
	{
		TestTrue(
			FString::Printf(TEXT("shard '%s' must not exceed capacity (got %d)"), *Pair.Key, Pair.Value),
			Pair.Value <= Policy.MaxMeshesPerShard);
	}
	// 至少应该按 cluster 分出 N 个 shard。
	TestTrue(
		FString::Printf(TEXT("auto split should produce >= %d shards (got %d)"), ClusterCount, CountByShard.Num()),
		CountByShard.Num() >= ClusterCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualShardMaxDepthFallbackTest,
	"Monolith.Index.AssetVisual.Sharding.MaxDepthFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualShardMaxDepthFallbackTest::RunTest(const FString& Parameters)
{
	// 验证算法的"路径耗尽时按 warning 接受过载 shard"行为：
	// 50 mesh 全部塞进同一个叶子目录（路径已无更深层可拆），
	// 算法应当达到 MaxPrefixDepth 后停下并接受这个超容量 shard，而不是无限循环或崩溃。
	TArray<FString> Paths;
	for (int32 Index = 0; Index < 50; ++Index)
	{
		Paths.Add(FString::Printf(TEXT("/Game/Vehicles/Cars/SM_Car_%03d.SM_Car_%03d"), Index, Index));
	}

	FAssetVisualShardCapacityPolicy Policy;
	Policy.InitialPrefixDepth = 2;
	Policy.MaxMeshesPerShard = 10; // 故意小于 50，且路径无法再拆
	Policy.MaxPrefixDepth = 6;

	// 该测试预期会触发"路径耗尽"warning，加 expected error 让 automation 不把 warning 算作失败。
	AddExpectedError(TEXT("AssetVisual shard"), EAutomationExpectedErrorFlags::Contains, 0, /*bIsRegex=*/false);

	const TMap<FString, FAssetVisualShardKey> Assignment = AssignAssetVisualShardsForCohort(Paths, Policy);
	TestEqual(TEXT("all paths still assigned at fallback"), Assignment.Num(), Paths.Num());

	// 全部 50 mesh 必须落进同一个 shard（或少数几个），不能丢。
	TMap<FString, int32> CountByShard;
	for (const TPair<FString, FAssetVisualShardKey>& Pair : Assignment)
	{
		++CountByShard.FindOrAdd(Pair.Value.ShardId);
	}
	int32 TotalCount = 0;
	for (const TPair<FString, int32>& Pair : CountByShard)
	{
		TotalCount += Pair.Value;
	}
	TestEqual(TEXT("no mesh dropped at fallback"), TotalCount, 50);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualShardAssignmentStableTest,
	"Monolith.Index.AssetVisual.Sharding.AssignmentStableAcrossRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualShardAssignmentStableTest::RunTest(const FString& Parameters)
{
	// Horde 多机协作前提：同样输入两次跑必须 bit-identical 输出。
	TArray<FString> Paths = {
		TEXT("/Game/Buildings/Houses/SM_A.SM_A"),
		TEXT("/Game/Buildings/Shops/SM_B.SM_B"),
		TEXT("/Game/Vehicles/Cars/SM_C.SM_C"),
		TEXT("/Plugin/Foo/Bar/SM_D.SM_D"),
	};

	FAssetVisualShardCapacityPolicy Policy;
	Policy.InitialPrefixDepth = 2;
	Policy.MaxMeshesPerShard = 1000;
	Policy.MaxPrefixDepth = 6;

	const TMap<FString, FAssetVisualShardKey> A = AssignAssetVisualShardsForCohort(Paths, Policy);
	const TMap<FString, FAssetVisualShardKey> B = AssignAssetVisualShardsForCohort(Paths, Policy);

	for (const TPair<FString, FAssetVisualShardKey>& Pair : A)
	{
		const FAssetVisualShardKey* Other = B.Find(Pair.Key);
		TestNotNull(TEXT("path missing in second run"), Other);
		if (Other)
		{
			TestEqual(TEXT("shard id must match across runs"), Pair.Value.ShardId, Other->ShardId);
			TestEqual(TEXT("prefix depth must match across runs"), Pair.Value.PrefixDepth, Other->PrefixDepth);
		}
	}
	return true;
}
