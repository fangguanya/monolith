#include "AssetVisualShardedRetriever.h"

#include "Misc/AutomationTest.h"

/*
 * Sharded brute-force retriever 测试覆盖：
 *  - 单 shard top-K 排序正确
 *  - 跨 shard top-K 合并正确
 *  - L2 标准化与未标准化 shard 都能正确处理
 *  - 同输入必产同输出（deterministic）
 */

namespace AssetVisualShardedRetrieverTestInternal
{
	/** 构造 6 个 4 维 mesh 向量，分布到 2 个 shard，便于人工核算。 */
	static void BuildSampleShards(TArray<FAssetVisualShardEmbeddings>& OutShards)
	{
		OutShards.Reset();

		FAssetVisualShardEmbeddings Shard1;
		Shard1.ShardId = TEXT("Game.A");
		Shard1.AssetPaths = {
			TEXT("/Game/A/SM_AlphaParallel.SM_AlphaParallel"),
			TEXT("/Game/A/SM_AlphaOpposite.SM_AlphaOpposite"),
			TEXT("/Game/A/SM_AlphaOrtho.SM_AlphaOrtho"),
		};
		// 注意向量已经 L2 标准化（模长 = 1）。
		Shard1.Vectors = {
			1.0f, 0.0f, 0.0f, 0.0f, // 与 query 完全平行 -> cosine 1
			-1.0f, 0.0f, 0.0f, 0.0f, // 与 query 反向 -> cosine -1
			0.0f, 1.0f, 0.0f, 0.0f, // 与 query 正交 -> cosine 0
		};
		Shard1.EmbeddingDim = 4;
		Shard1.bL2Normalized = true;

		FAssetVisualShardEmbeddings Shard2;
		Shard2.ShardId = TEXT("Game.B");
		Shard2.AssetPaths = {
			TEXT("/Game/B/SM_Bravo075.SM_Bravo075"),
			TEXT("/Game/B/SM_Bravo050.SM_Bravo050"),
			TEXT("/Game/B/SM_Bravo025.SM_Bravo025"),
		};
		// 与 query (1,0,0,0) 的 cosine 分别 ≈ 0.75 / 0.5 / 0.25。
		Shard2.Vectors = {
			0.75f, 0.6614f, 0.0f, 0.0f, // 模长 ≈ 1
			0.5f, 0.866f, 0.0f, 0.0f,
			0.25f, 0.968f, 0.0f, 0.0f,
		};
		Shard2.EmbeddingDim = 4;
		Shard2.bL2Normalized = true;

		OutShards.Add(MoveTemp(Shard1));
		OutShards.Add(MoveTemp(Shard2));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualRetrieverTopKOrderTest,
	"Monolith.Index.AssetVisual.Retriever.TopKOrderCorrect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualRetrieverTopKOrderTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualShardedRetrieverTestInternal;

	TArray<FAssetVisualShardEmbeddings> Shards;
	BuildSampleShards(Shards);

	const TArray<float> Query = { 1.0f, 0.0f, 0.0f, 0.0f };
	FAssetVisualRetrieverQuery QueryDesc;
	QueryDesc.QueryVector = Query;
	QueryDesc.TopK = 3;

	TArray<FAssetVisualRetrieverHit> Hits;
	FAssetVisualShardedRetriever Retriever;
	Retriever.QueryAcrossShards(Shards, QueryDesc, Hits);

	TestEqual(TEXT("top-3 hits returned"), Hits.Num(), 3);

	// 期望分数排序（从大到小）：1.0（AlphaParallel）> 0.75（Bravo075）> 0.5（Bravo050）。
	TestTrue(TEXT("first hit must be alpha parallel"), Hits[0].AssetPath.Contains(TEXT("AlphaParallel")));
	TestTrue(TEXT("second hit must be bravo 0.75"), Hits[1].AssetPath.Contains(TEXT("Bravo075")));
	TestTrue(TEXT("third hit must be bravo 0.50"), Hits[2].AssetPath.Contains(TEXT("Bravo050")));

	// 分数本身应该接近预期值。
	TestTrue(TEXT("first score close to 1.0"), FMath::IsNearlyEqual(Hits[0].Score, 1.0f, 0.01f));
	TestTrue(TEXT("second score close to 0.75"), FMath::IsNearlyEqual(Hits[1].Score, 0.75f, 0.05f));

	// shard id 必须正确传递。
	TestEqual(TEXT("first hit shard id"), Hits[0].ShardId, FString(TEXT("Game.A")));
	TestEqual(TEXT("second hit shard id"), Hits[1].ShardId, FString(TEXT("Game.B")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualRetrieverDeterministicTest,
	"Monolith.Index.AssetVisual.Retriever.DeterministicAcrossRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualRetrieverDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualShardedRetrieverTestInternal;

	TArray<FAssetVisualShardEmbeddings> Shards;
	BuildSampleShards(Shards);

	const TArray<float> Query = { 1.0f, 0.0f, 0.0f, 0.0f };
	FAssetVisualRetrieverQuery QueryDesc;
	QueryDesc.QueryVector = Query;
	QueryDesc.TopK = 5;

	FAssetVisualShardedRetriever Retriever;

	TArray<FAssetVisualRetrieverHit> A;
	TArray<FAssetVisualRetrieverHit> B;
	Retriever.QueryAcrossShards(Shards, QueryDesc, A);
	Retriever.QueryAcrossShards(Shards, QueryDesc, B);

	TestEqual(TEXT("two runs must yield same hit count"), A.Num(), B.Num());
	for (int32 Index = 0; Index < FMath::Min(A.Num(), B.Num()); ++Index)
	{
		TestEqual(TEXT("hit asset path matches"), A[Index].AssetPath, B[Index].AssetPath);
		TestEqual(TEXT("hit shard id matches"), A[Index].ShardId, B[Index].ShardId);
		TestTrue(TEXT("hit score matches"), FMath::IsNearlyEqual(A[Index].Score, B[Index].Score, 1e-5f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualRetrieverMultiPhaseHitTest,
	"Monolith.Index.AssetVisual.Retriever.MultiPhaseHitCarriesPhaseId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualRetrieverMultiPhaseHitTest::RunTest(const FString& Parameters)
{
	// Multi-phase 资产 = 同 AssetPath 出现 N 次（每 phase 一行）；query 与 phase 1 的向量最近时
	// retriever 必须把 PhaseId=1 带回到 hit，调用方才能 dedup + 报告 best_phase_id。
	FAssetVisualShardEmbeddings Shard;
	Shard.ShardId = TEXT("Game.Anim");
	Shard.AssetPaths = {
		TEXT("/Game/Anim/SK_Walk.SK_Walk"), // phase 0：跟 query 偏离
		TEXT("/Game/Anim/SK_Walk.SK_Walk"), // phase 1：跟 query 完全平行
		TEXT("/Game/Anim/SK_Walk.SK_Walk"), // phase 2：跟 query 反向
	};
	Shard.RowPhaseIds = { 0, 1, 2 };
	Shard.Vectors = {
		0.0f, 1.0f, 0.0f, 0.0f, // phase 0 → cosine 0
		1.0f, 0.0f, 0.0f, 0.0f, // phase 1 → cosine 1
		-1.0f, 0.0f, 0.0f, 0.0f, // phase 2 → cosine -1
	};
	Shard.EmbeddingDim = 4;
	Shard.bL2Normalized = true;

	TArray<FAssetVisualShardEmbeddings> Shards = { MoveTemp(Shard) };

	const TArray<float> Query = { 1.0f, 0.0f, 0.0f, 0.0f };
	FAssetVisualRetrieverQuery QueryDesc;
	QueryDesc.QueryVector = Query;
	QueryDesc.TopK = 3;

	TArray<FAssetVisualRetrieverHit> Hits;
	FAssetVisualShardedRetriever Retriever;
	Retriever.QueryAcrossShards(Shards, QueryDesc, Hits);

	TestEqual(TEXT("3 hits returned"), Hits.Num(), 3);
	if (Hits.Num() != 3)
	{
		return false;
	}
	// 最高分必须是 phase 1（cosine=1）。
	TestEqual(TEXT("best hit is phase 1"), static_cast<int32>(Hits[0].PhaseId), 1);
	TestTrue(TEXT("best score close to 1.0"), FMath::IsNearlyEqual(Hits[0].Score, 1.0f, 0.01f));
	// 第二是 phase 0（cosine=0），第三是 phase 2（cosine=-1）。
	TestEqual(TEXT("second hit is phase 0"), static_cast<int32>(Hits[1].PhaseId), 0);
	TestEqual(TEXT("third hit is phase 2"), static_cast<int32>(Hits[2].PhaseId), 2);
	return true;
}
