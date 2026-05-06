#pragma once

#include "CoreMinimal.h"

/*
 * AssetVisual sharded brute-force cosine retriever。
 *
 * 算法选型背书（写在头文件，避免后人误以为这是临时实现）：
 *
 *  N = 50,000 mesh / shard, D = 64 (geometric) or 512 (semantic)
 *  brute-force cosine = N × 2D FLOPs ≈ 6.4M (geo) / 51M (semantic) per shard
 *  现代 x86 SIMD 单核 ~10–30 GFLOPs ⇒ 0.5–2ms (geo) / 5–15ms (semantic)
 *  全 cohort 30 shard 并行 ⇒ wall time ~3ms (geo) / ~10ms (semantic)
 *
 * 在这个规模下，brute-force 是数学最优的 ANN 算法，不是简化方案：
 *  - HNSW / faiss 在 N ≤ 100K 没有显著加速；
 *  - HNSW 召回率 < 100% 且需要调参；
 *  - HNSW 引入第三方依赖、license 与复杂度；
 *  - sharded brute-force 100% 召回 + bit-identical + 单元测试可断言。
 *
 * 真要 N > 1M 且 single shard 也会变大，再换 HNSW 是干净的"换实现"，不会破坏外部接口。
 */

/** ANN 召回输入：查询向量 + 想要的候选数。 */
struct MONOLITHINDEX_API FAssetVisualRetrieverQuery
{
	/** 查询向量；维度必须与 cohort 内 embedding 维度完全一致。 */
	TArrayView<const float> QueryVector;
	/** 想要的 top-K 数量；retriever 会 clamp 到 [1, 100]。 */
	int32 TopK = 10;
};

/** 单个 shard 的输入数据；retriever 不负责加载或缓存，调用方按需提供。 */
struct MONOLITHINDEX_API FAssetVisualShardEmbeddings
{
	/** shard 标识符；输出结果会带上以便追溯。 */
	FString ShardId;
	/** shard 内 mesh asset path 列表；与 Vectors 一一对应，长度必须相等。
	 *  Multi-phase 资产同 AssetPath 重复出现 N 次（每 phase 一行）。 */
	TArray<FString> AssetPaths;
	/** 平铺向量数组：每 EmbeddingDim 个 float 表示一个 mesh 的 embedding。 */
	TArray<float> Vectors;
	/** 与 AssetPaths 一一对应的 phase 序号（0..N-1）。 */
	TArray<uint8> RowPhaseIds;
	/** 单个 embedding 的维度。 */
	int32 EmbeddingDim = 0;
	/** Vectors 是否已经被 L2 标准化；标准化后 retriever 不再除模长。 */
	bool bL2Normalized = true;
};

/** 单个候选结果。 */
struct MONOLITHINDEX_API FAssetVisualRetrieverHit
{
	/** 命中的 mesh asset path。 */
	FString AssetPath;
	/** cosine similarity 分数（标准化后即 dot product），范围 [-1, 1]。 */
	float Score = 0.0f;
	/** 该候选所在的 shard id。 */
	FString ShardId;
	/** 命中行的 phase 序号；单 phase 资产恒为 0，多 phase 资产 = 该次命中对应的相位。 */
	uint8 PhaseId = 0;
};

/*
 * Sharded brute-force cosine retriever。
 *
 * 用法：
 *   FAssetVisualShardedRetriever Retriever;
 *   TArray<FAssetVisualRetrieverHit> Hits;
 *   Retriever.QueryAcrossShards(Shards, Query, Hits);
 *
 * 特性：
 *  - 内部用 ParallelFor 跨 shard 并行；
 *  - 每 shard 算 shard-local top-K，最后汇总成 cohort-level top-K；
 *  - 输入未 L2 标准化时自动归一化（多花一次除法）；
 *  - 同输入必产同输出，可在自动化测试里直接 TestEqual 比较。
 */
class MONOLITHINDEX_API FAssetVisualShardedRetriever
{
public:
	/** 跨多 shard 召回 top-K。Shards 与 Query 都不被修改。 */
	void QueryAcrossShards(
		const TArray<FAssetVisualShardEmbeddings>& Shards,
		const FAssetVisualRetrieverQuery& Query,
		TArray<FAssetVisualRetrieverHit>& OutHits) const;
};

/** 工具函数：把任意向量原地 L2 标准化；零向量保持不变。 */
MONOLITHINDEX_API void L2NormalizeInPlace(TArrayView<float> Vector);
