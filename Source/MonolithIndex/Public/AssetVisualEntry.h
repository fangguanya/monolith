#pragma once

#include "CoreMinimal.h"

/*
 * 这份头文件定义 AssetVisual cohort 的两种本地行模型。
 *
 *  - FIndexedAssetVisualEntry：单 mesh 在某个视觉 cohort 内的一行，包含 embedding 与 preview 路径
 *  - FMonolithShadowIndexedAssetVisualEntry：shadow 表对应的影子行 + 行哈希
 *
 * geometric / semantic 两个 cohort 共用同一份结构（embedding 字节数 + provider triple 不同），
 * 因为它们的存储语义完全相同，只是物理上写到两张不同的表里。
 */

/** 单 mesh 在视觉 cohort 内的一行；同时承载 embedding 与渲染元信息。 */
struct FIndexedAssetVisualEntry
{
	/** 主键。 */
	int64 Id = 0;
	/** 属于哪个静态网格资产。 */
	int64 AssetId = 0;
	/** 属于哪个 revision。 */
	int64 RevisionId = 0;
	/** 资产对象路径，例如 /Game/Props/SM_Chair.SM_Chair。 */
	FString AssetPath;
	/** 该 mesh 所属 shard 的稳定标识符（与 ANN 快照按 shard 切片对齐）。 */
	FString ShardId;
	/** 该 shard 计算时使用的 path 前缀段数；用于复算 shard 边界。 */
	int32 ShardPrefixDepth = 0;
	/** Provider id，例如 `geometric_v1` / `clip_vit_b32_v1`。 */
	FString ProviderId;
	/** Provider 版本号；任何变化触发 cohort stale。 */
	uint32 ProviderVersion = 1;
	/** RenderRecipe 版本号；任何渲染参数变化触发 cohort stale。 */
	uint32 RenderRecipeVersion = 1;
	/** Embedding 维度，与 EmbeddingBytes 对应；geometric=64, semantic=512。 */
	int32 EmbeddingDim = 0;
	/** Embedding 的存储格式标记：0=FP32, 1=FP16；用于解释 EmbeddingBytes。 */
	uint8 EmbeddingDtype = 0;
	/** Embedding 字节数组：FP32 时长度 = EmbeddingDim*4；FP16 时长度 = EmbeddingDim*2。 */
	TArray<uint8> EmbeddingBytes;
	/** 持久化的 256×256 iso 预览 PNG 路径；query 结果的 `preview_view` 字段直接拿这个。 */
	FString PreviewViewPath;

	/*
	 * Multi-phase 索引字段：让 VFX/Anim 等"时序资产"按多个时间点分别落 embedding 行。
	 *
	 *  - 单 phase 资产（StaticMesh/Material 等）：PhaseId=0, PhaseT=0, PhaseLabel="" 全默认；
	 *    一个 asset 在 cohort 内仍是一行。
	 *  - 多 phase 资产（AnimSequence/Niagara）：同一 asset 在 cohort 内有多行，
	 *    PhaseId=0..N-1 对应 GetPhasesForAsset 给出的相位序号；search 端按 asset_path GROUP BY 取最高分。
	 *
	 * 三个字段一起决定一行的 phase 身份：PhaseId 是稳定整数键，PhaseT 是采样时间（秒或归一化），
	 * PhaseLabel 是给搜索结果用的可读名（"early"/"peak"/"tail"）。
	 */
	uint8 PhaseId = 0;
	float PhaseT = 0.0f;
	FString PhaseLabel;
};

/** Shadow 模式下的视觉行；额外带稳定 row hash。 */
struct FMonolithShadowIndexedAssetVisualEntry
{
	/** 真正的视觉行数据。 */
	FIndexedAssetVisualEntry Entry;
	/** 用于 Level 1 / Level 2 diff 的稳定行哈希。 */
	uint64 RowHash = 0;
};

/** 视觉 cohort 的聚合摘要，用于 shadow mode Level 1 diff。 */
struct FMonolithShadowAssetVisualAggregate
{
	uint64 RowCount = 0;
	uint64 RowHashSum = 0;
};
