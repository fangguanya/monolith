#include "AssetVisualShardedRetriever.h"

#include "Async/ParallelFor.h"
#include "Containers/Queue.h"
#include "MonolithIndexLog.h"

/*
 * 实现细节集中在两件事：
 *  1. 单 shard 内的 brute-force top-K：用 partial heap 而非全排序，O(N log K)
 *  2. 跨 shard 的并行：ParallelFor 在 Background 池上跑，单 shard 任务约 1–10ms
 *
 * 任何能在不引入第三方库前提下显著提升性能的优化（SIMD intrinsics 等）都欢迎，
 * 但必须保证算法语义不变（cosine = dot of L2-normalized vectors）。
 */
namespace AssetVisualShardedRetrieverInternal
{
	/** 单 shard 内对某个查询向量做 brute-force top-K cosine 召回。 */
	static void QuerySingleShard(
		const FAssetVisualShardEmbeddings& Shard,
		const TArrayView<const float> QueryVector,
		const int32 TopK,
		TArray<FAssetVisualRetrieverHit>& OutShardHits)
	{
		OutShardHits.Reset();

		if (Shard.EmbeddingDim <= 0 || Shard.AssetPaths.Num() == 0 || Shard.Vectors.Num() == 0)
		{
			return;
		}

		const int32 ExpectedFloatCount = Shard.AssetPaths.Num() * Shard.EmbeddingDim;
		if (Shard.Vectors.Num() != ExpectedFloatCount)
		{
			UE_LOG(LogMonolithIndex, Error,
				TEXT("AssetVisualShardedRetriever shard '%s' 向量数组长度不匹配：期望 %d 实际 %d"),
				*Shard.ShardId, ExpectedFloatCount, Shard.Vectors.Num());
			return;
		}

		if (QueryVector.Num() != Shard.EmbeddingDim)
		{
			UE_LOG(LogMonolithIndex, Error,
				TEXT("AssetVisualShardedRetriever 查询向量维度 %d 与 shard '%s' 维度 %d 不一致"),
				QueryVector.Num(), *Shard.ShardId, Shard.EmbeddingDim);
			return;
		}

		// 按 dot product 评分；如果 shard 没有事先 L2 标准化，函数侧会一次性除以向量模长。
		const int32 NumVectors = Shard.AssetPaths.Num();
		const int32 Dim = Shard.EmbeddingDim;

		// 用 reservoir-style partial heap 维护 top-K：堆顶最小值；新分数大于堆顶才入堆。
		struct FHeapEntry
		{
			float Score;
			int32 Index;
		};
		TArray<FHeapEntry> Heap;
		const int32 ClampedK = FMath::Clamp(TopK, 1, NumVectors);
		Heap.Reserve(ClampedK);

		const float* VectorBase = Shard.Vectors.GetData();
		const float* QueryBase = QueryVector.GetData();

		for (int32 VectorIndex = 0; VectorIndex < NumVectors; ++VectorIndex)
		{
			const float* Vector = VectorBase + VectorIndex * Dim;

			// dot product 累加；编译器有机会自动向量化。
			float Dot = 0.0f;
			for (int32 D = 0; D < Dim; ++D)
			{
				Dot += Vector[D] * QueryBase[D];
			}

			float Score = Dot;
			if (!Shard.bL2Normalized)
			{
				// 临时算模长，仅在 shard 未事先标准化时执行。
				float NormSq = 0.0f;
				for (int32 D = 0; D < Dim; ++D)
				{
					NormSq += Vector[D] * Vector[D];
				}
				const float Norm = FMath::Sqrt(NormSq);
				if (Norm > 1e-6f)
				{
					Score = Dot / Norm;
				}
				else
				{
					// 零向量：cosine 未定义，给最低分让它不进 top-K。
					Score = -1.0f;
				}
			}

			if (Heap.Num() < ClampedK)
			{
				Heap.Add({ Score, VectorIndex });
				if (Heap.Num() == ClampedK)
				{
					Heap.Heapify([](const FHeapEntry& A, const FHeapEntry& B) { return A.Score < B.Score; });
				}
				continue;
			}

			if (Score > Heap.HeapTop().Score)
			{
				FHeapEntry Popped;
				Heap.HeapPop(Popped, [](const FHeapEntry& A, const FHeapEntry& B) { return A.Score < B.Score; }, EAllowShrinking::No);
				Heap.HeapPush({ Score, VectorIndex }, [](const FHeapEntry& A, const FHeapEntry& B) { return A.Score < B.Score; });
			}
		}

		// 按分数从大到小输出 shard-local top-K。
		Heap.Sort([](const FHeapEntry& A, const FHeapEntry& B) { return A.Score > B.Score; });

		OutShardHits.Reserve(Heap.Num());
		for (const FHeapEntry& Entry : Heap)
		{
			FAssetVisualRetrieverHit Hit;
			Hit.AssetPath = Shard.AssetPaths[Entry.Index];
			Hit.Score = Entry.Score;
			Hit.ShardId = Shard.ShardId;
			OutShardHits.Add(MoveTemp(Hit));
		}
	}
}

void L2NormalizeInPlace(TArrayView<float> Vector)
{
	float NormSq = 0.0f;
	for (const float V : Vector)
	{
		NormSq += V * V;
	}
	const float Norm = FMath::Sqrt(NormSq);
	if (Norm <= 1e-6f)
	{
		// 零向量保持不变（标准化后还是零向量）；调用方应在 ANN 召回时把这种向量过滤掉。
		return;
	}
	const float InvNorm = 1.0f / Norm;
	for (float& V : Vector)
	{
		V *= InvNorm;
	}
}

void FAssetVisualShardedRetriever::QueryAcrossShards(
	const TArray<FAssetVisualShardEmbeddings>& Shards,
	const FAssetVisualRetrieverQuery& Query,
	TArray<FAssetVisualRetrieverHit>& OutHits) const
{
	using namespace AssetVisualShardedRetrieverInternal;

	OutHits.Reset();

	if (Shards.Num() == 0 || Query.QueryVector.Num() == 0)
	{
		return;
	}

	const int32 ClampedTopK = FMath::Clamp(Query.TopK, 1, 100);

	// 查询向量本身必须是 L2-normalized；如果调用方还没归一化，retriever 帮一次。
	// 这里不修改外部传入的 ArrayView，而是按需 copy，否则会在多线程查询时引入数据竞争。
	TArray<float> NormalizedQuery;
	NormalizedQuery.Append(Query.QueryVector.GetData(), Query.QueryVector.Num());
	L2NormalizeInPlace(NormalizedQuery);
	const TArrayView<const float> QueryView(NormalizedQuery);

	// 跨 shard 并行：每个 shard 算 shard-local top-K，写到自己专属的输出槽位。
	TArray<TArray<FAssetVisualRetrieverHit>> PerShardHits;
	PerShardHits.SetNum(Shards.Num());

	ParallelFor(Shards.Num(), [&](const int32 ShardIndex)
	{
		QuerySingleShard(Shards[ShardIndex], QueryView, ClampedTopK, PerShardHits[ShardIndex]);
	});

	// 合并 shard-local top-K 成 cohort-level top-K：按分数从大到小重新排，再截前 ClampedTopK。
	int32 TotalHits = 0;
	for (const TArray<FAssetVisualRetrieverHit>& ShardHits : PerShardHits)
	{
		TotalHits += ShardHits.Num();
	}
	OutHits.Reserve(TotalHits);
	for (TArray<FAssetVisualRetrieverHit>& ShardHits : PerShardHits)
	{
		OutHits.Append(MoveTemp(ShardHits));
	}

	OutHits.Sort([](const FAssetVisualRetrieverHit& A, const FAssetVisualRetrieverHit& B)
	{
		return A.Score > B.Score;
	});

	if (OutHits.Num() > ClampedTopK)
	{
		OutHits.SetNum(ClampedTopK, EAllowShrinking::No);
	}
}
