#pragma once

#include "CoreMinimal.h"

/*
 * AssetVisual sharding 抽象。
 *
 * 设计动机：
 * - 在 50 万级 mesh 规模下，单个全局 ANN 快照会远超 ADR oversized_artifact 16MB 上限，
 *   也无法在 30ms p95 预算内完成 brute-force 召回。
 * - 把 mesh 按 asset path 前缀分 shard 之后，每 shard 内部仍然走 brute-force（在该规模下是
 *   数学上的正确算法），shard 之间天然并行，单个 shard ANN 快照天然 ≤16MB。
 *
 * 这份头文件只定义 shard key 的计算规则与容量策略，不假设任何具体 cohort 的存储或检索实现。
 * AssetVisualGeometric 与 AssetVisualSemantic 各自带不同的容量上限，但 shard 计算函数共用同一份。
 */

/** 单 cohort 的 shard 容量策略，在容量超过上限时按下一段路径自动再拆。 */
struct MONOLITHINDEX_API FAssetVisualShardCapacityPolicy
{
	/** 起步使用的 path 前缀段数，默认 2，对应 `/Game/<L1>/<L2>`。 */
	int32 InitialPrefixDepth = 2;
	/** shard 容量上限（mesh 数）；超过此上限必须按下一段路径再拆。 */
	int32 MaxMeshesPerShard = 50000;
	/** 路径段数自动再拆的最大层数；保护极端倾斜分布不会把 shard 切成无意义碎片。 */
	int32 MaxPrefixDepth = 6;

	/** geometric cohort 默认策略：64 维向量，单 shard 50K mesh 时 ANN 快照 ≈12.8MB。 */
	static FAssetVisualShardCapacityPolicy Geometric()
	{
		FAssetVisualShardCapacityPolicy Policy;
		Policy.InitialPrefixDepth = 2;
		Policy.MaxMeshesPerShard = 50000;
		Policy.MaxPrefixDepth = 6;
		return Policy;
	}

	/** semantic cohort 默认策略：512 维 FP16 向量，单 shard 8K mesh 时 ANN 快照 ≈8MB。 */
	static FAssetVisualShardCapacityPolicy Semantic()
	{
		FAssetVisualShardCapacityPolicy Policy;
		Policy.InitialPrefixDepth = 2;
		Policy.MaxMeshesPerShard = 8000;
		Policy.MaxPrefixDepth = 6;
		return Policy;
	}
};

/** 单 mesh 的 shard 计算结果。 */
struct MONOLITHINDEX_API FAssetVisualShardKey
{
	/** 稳定的 shard 标识符，例如 `Game.Buildings.Houses`。
	 * 不直接用斜杠是为了让它能安全地拼进 DDC bucket / 文件名。 */
	FString ShardId;
	/** 实际生成 ShardId 时使用的 path 前缀段数。 */
	int32 PrefixDepth = 0;

	bool operator==(const FAssetVisualShardKey& Other) const
	{
		return ShardId == Other.ShardId && PrefixDepth == Other.PrefixDepth;
	}
};

FORCEINLINE uint32 GetTypeHash(const FAssetVisualShardKey& Key)
{
	return HashCombine(GetTypeHash(Key.ShardId), GetTypeHash(Key.PrefixDepth));
}

/*
 * 给一份资产路径计算其 shard key。
 *
 * @param AssetPath          完整资产路径，例如 `/Game/Buildings/Houses/SM_House.SM_House`
 * @param PrefixDepth        要使用的路径前缀段数；遵守 `Policy.MaxPrefixDepth` 上限
 *
 * 规则：
 * - `/Game/<L1>/<L2>/...` 截前 PrefixDepth 段：例如 PrefixDepth=2 -> `Game.Buildings.Houses` 不对，
 *   实际是 `Game.Buildings`（前 2 段），PrefixDepth=3 才会得到 `Game.Buildings.Houses`。
 * - `/<Plugin>/<L1>/...` 同样按段数截取。
 * - 路径段数不足时，剩余段用空字符串 `_` 占位，保证 shard key 始终对齐 PrefixDepth。
 * - 不在 `/Game/` 下的路径同样按 PrefixDepth 处理。
 *
 * 该函数完全 deterministic，同一资产同一 PrefixDepth 必定得到同一 ShardId。
 */
MONOLITHINDEX_API FAssetVisualShardKey ComputeAssetVisualShardKey(
	const FString& AssetPath,
	int32 PrefixDepth);

/*
 * 在容量分布已知时，按策略给一组 mesh 自动选定最浅但满足容量上限的 shard 划分方案。
 *
 * @param AssetPaths   全部待分 shard 的资产路径
 * @param Policy       cohort 的容量策略
 * @return             资产路径 -> 最终 ShardKey 的映射
 *
 * 算法：
 * 1. 从 InitialPrefixDepth 起步，统计每个 shard 当前包含多少 mesh
 * 2. 把超容量的 shard 按下一段 PrefixDepth 重新展开，未超容量的 shard 保持原层
 * 3. 直到所有 shard 都满足容量上限，或达到 MaxPrefixDepth（继续超就停在最深一层并记 warning）
 *
 * 此函数纯 CPU 计算，输入相同必产出完全相同的映射，可在 Horde agent / 编辑器 / 自动化测试间复用。
 */
MONOLITHINDEX_API TMap<FString, FAssetVisualShardKey> AssignAssetVisualShardsForCohort(
	const TArray<FString>& AssetPaths,
	const FAssetVisualShardCapacityPolicy& Policy);
