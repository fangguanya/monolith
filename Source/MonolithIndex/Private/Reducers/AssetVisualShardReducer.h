#pragma once

#include "CoreMinimal.h"
#include "AssetVisualEntry.h"
#include "AssetVisualSharding.h"
#include "AssetVisualShardedRetriever.h"
#include "MonolithArtifactCache.h"

class FMonolithIndexDatabase;

/*
 * AssetVisualShardReducer：把单个 cohort 的视觉行按 shard 聚合成 ANN 快照 artifact。
 *
 * geometric / semantic 两个 cohort 共用这一份 reducer 实现，区别只在：
 *  - CohortName  ：决定从哪张 SQLite 表读
 *  - DdcCache    ：决定写到哪个 bucket
 *  - Capacity    ：决定 shard 容量上限
 *
 * 一份 shard ANN 快照 artifact = 该 shard 全部 mesh 的 (asset_path, embedding) 平铺数组。
 * Shard 自动再拆策略由 AssignAssetVisualShardsForCohort 完成；reducer 仅负责"按 shard 写一次"。
 *
 * 调用方典型用法（warmup commandlet 里）：
 *   FAssetVisualShardReducer Reducer;
 *   Reducer.RebuildAllShards(DB, Cache, TEXT("AssetVisualGeometric"),
 *       FAssetVisualShardCapacityPolicy::Geometric());
 *
 * Reducer 全程纯 CPU + 顺序 SQLite 读 + 顺序 DDC 写，可在 BackgroundCpuPool 内执行。
 */
struct MONOLITHINDEX_API FAssetVisualShardReducerStats
{
	int32 ShardCount = 0;
	int32 ShardsWritten = 0;
	int32 ShardsSkipped = 0;
	int32 EntriesProcessed = 0;
};

class MONOLITHINDEX_API FAssetVisualShardReducer
{
public:
	/** 全量重建某 cohort 的所有 shard 快照，写到 DDC bucket。
	 *  reducer 仅负责把 SQLite 中的视觉行按 shard 聚合落 DDC；
	 *  query 路径不依赖 reducer 输出，而是直接从 SQLite 加载 mesh 行后跑 brute-force。
	 *  reducer 输出的真正用途是 Horde / 跨机预热，让 agent 不必本地有 SQLite 也能消费 shard 快照。 */
	FAssetVisualShardReducerStats RebuildAllShards(
		FMonolithIndexDatabase& DB,
		IMonolithArtifactCache& DdcCache,
		const FString& CohortName,
		const FAssetVisualShardCapacityPolicy& Policy);
};
