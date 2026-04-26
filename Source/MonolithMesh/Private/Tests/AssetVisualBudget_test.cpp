#include "AssetVisualShardedRetriever.h"

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

/*
 * AssetVisual 性能预算测试。
 *
 * 这些断言保证在 50K mesh × 64 维 / 8K mesh × 512 维 cohort 规模下，
 * spec 锁死的 ANN 召回 30ms p95 不会偷偷退化。
 *
 * 测试方式：构造合成 shard 数据，运行 brute-force retriever，统计 wall-clock。
 * 不依赖任何实际 mesh 数据或 GPU；纯 CPU + 内存基准。
 */

namespace AssetVisualBudgetTestInternal
{
	/** 构造 N 维 D 长度的 deterministic 浮点向量（可复现）。 */
	static void BuildShardWithRandomVectors(
		const FString& ShardId,
		const int32 EmbeddingDim,
		const int32 EntryCount,
		FAssetVisualShardEmbeddings& OutShard)
	{
		OutShard.ShardId = ShardId;
		OutShard.EmbeddingDim = EmbeddingDim;
		OutShard.bL2Normalized = true;
		OutShard.AssetPaths.Reserve(EntryCount);
		OutShard.Vectors.SetNumUninitialized(EntryCount * EmbeddingDim);

		uint32 RngState = 0x13579BDFu;
		auto NextFloat = [&]()
		{
			RngState = RngState * 1664525u + 1013904223u;
			const uint32 Bits = (RngState >> 9) | 0x3f800000u; // [1, 2)
			float V = 0.0f;
			FMemory::Memcpy(&V, &Bits, sizeof(float));
			return V - 1.5f;
		};

		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			OutShard.AssetPaths.Add(FString::Printf(TEXT("/Game/Synth/Mesh_%05d.Mesh_%05d"), Index, Index));
			float Norm = 0.0f;
			for (int32 D = 0; D < EmbeddingDim; ++D)
			{
				const float V = NextFloat();
				OutShard.Vectors[Index * EmbeddingDim + D] = V;
				Norm += V * V;
			}
			Norm = FMath::Sqrt(FMath::Max(Norm, 1e-6f));
			for (int32 D = 0; D < EmbeddingDim; ++D)
			{
				OutShard.Vectors[Index * EmbeddingDim + D] /= Norm;
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualAnnGeometricBudgetTest,
	"Monolith.Mesh.AssetVisual.Budget.AnnGeometricUnder30ms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualAnnGeometricBudgetTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualBudgetTestInternal;

	// 构造 1 个 shard，50K mesh × 64 维（geometric cohort 单 shard 最坏情况）。
	FAssetVisualShardEmbeddings Shard;
	BuildShardWithRandomVectors(TEXT("Synth.Geometric"), /*Dim=*/64, /*N=*/50000, Shard);

	const TArray<FAssetVisualShardEmbeddings> Shards = { MoveTemp(Shard) };

	// 查询向量
	TArray<float> Query;
	Query.SetNumZeroed(64);
	Query[0] = 1.0f;

	FAssetVisualRetrieverQuery Q;
	Q.QueryVector = Query;
	Q.TopK = 10;

	FAssetVisualShardedRetriever Retriever;
	TArray<FAssetVisualRetrieverHit> Hits;

	// 跑 5 次取最大 wall-clock，作为 p95 替身（spec 30ms）。
	double MaxMs = 0.0;
	for (int32 Try = 0; Try < 5; ++Try)
	{
		const double Start = FPlatformTime::Seconds();
		Retriever.QueryAcrossShards(Shards, Q, Hits);
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		MaxMs = FMath::Max(MaxMs, Ms);
	}

	UE_LOG(LogTemp, Log, TEXT("[AssetVisualBudget] geometric ANN single-shard 50K wall p95-ish = %.2f ms"), MaxMs);
	TestTrue(FString::Printf(TEXT("ANN p95 must stay <= 30ms (got %.2f ms)"), MaxMs), MaxMs <= 30.0);
	TestEqual(TEXT("returned exactly K hits"), Hits.Num(), 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualAnnSemanticBudgetTest,
	"Monolith.Mesh.AssetVisual.Budget.AnnSemanticUnder30ms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualAnnSemanticBudgetTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualBudgetTestInternal;

	// 1 个 shard，8K mesh × 512 维（semantic cohort 单 shard 最坏情况）。
	FAssetVisualShardEmbeddings Shard;
	BuildShardWithRandomVectors(TEXT("Synth.Semantic"), /*Dim=*/512, /*N=*/8000, Shard);

	const TArray<FAssetVisualShardEmbeddings> Shards = { MoveTemp(Shard) };

	TArray<float> Query;
	Query.SetNumZeroed(512);
	Query[0] = 1.0f;

	FAssetVisualRetrieverQuery Q;
	Q.QueryVector = Query;
	Q.TopK = 10;

	FAssetVisualShardedRetriever Retriever;
	TArray<FAssetVisualRetrieverHit> Hits;

	double MaxMs = 0.0;
	for (int32 Try = 0; Try < 5; ++Try)
	{
		const double Start = FPlatformTime::Seconds();
		Retriever.QueryAcrossShards(Shards, Q, Hits);
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		MaxMs = FMath::Max(MaxMs, Ms);
	}

	UE_LOG(LogTemp, Log, TEXT("[AssetVisualBudget] semantic ANN single-shard 8K wall p95-ish = %.2f ms"), MaxMs);
	TestTrue(FString::Printf(TEXT("ANN p95 must stay <= 30ms (got %.2f ms)"), MaxMs), MaxMs <= 30.0);
	TestEqual(TEXT("returned exactly K hits"), Hits.Num(), 10);
	return true;
}
