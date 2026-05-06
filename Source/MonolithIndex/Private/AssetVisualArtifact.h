#pragma once

#include "CoreMinimal.h"
#include "AssetVisualEntry.h"

/*
 * AssetVisual artifact 内部序列化协议（geometric / semantic 共用）。
 *
 * v3 多 phase + per-phase PNG layout：
 *   uint8  PayloadSchema = 3
 *   string AssetPath              // 共享：所有 phase 同一资产
 *   string ShardId                // 共享
 *   uint32 ShardPrefixDepth       // 共享
 *   string ProviderId             // 共享
 *   uint32 ProviderVersion        // 共享
 *   uint32 RenderRecipeVersion    // 共享
 *   uint32 EmbeddingDim           // 共享：所有 phase 同一维度
 *   uint8  EmbeddingDtype         // 共享：0=FP32, 1=FP16
 *   uint32 NumPhases
 *   for each phase:
 *     uint8  PhaseId
 *     uint32 PhaseT_bits          // IEEE-754 little-endian uint32 bit pattern
 *     string PhaseLabel
 *     uint32 EmbeddingByteCount
 *     bytes  EmbeddingBytes
 *     uint32 PreviewPngByteCount  // 0 表示该 phase 不携带 preview（semantic cohort 复用 geometric 的）
 *     bytes  PreviewPngBytes
 *
 * 写入约定：
 *  - geometric cohort 是 preview PNG 的 owner（先索引），artifact 携带每 phase 一张 PNG 字节
 *  - semantic cohort 复用 geometric 写下的 iso PNG，artifact 内每 phase PreviewPngByteCount=0
 *  - 多 phase 资产 PhaseId=0..N-1 各对应一张 PNG，落盘时文件名带 _pN 后缀
 *  - 单 phase 资产 PhaseId=0 仅一张 PNG，落盘文件名无后缀
 *
 * v1（单 entry，单 PNG）和 v2（多 phase 但共享一张 PNG）的旧 artifact 不再被 v3 deserialize 接受 ——
 * MonolithIndexDatabase 的 v10→v11 migration 已经清空生产表，IndexerVersion bump 让 DDC 失效，
 * 强制下一次 Materialize 整体重建。
 */
namespace AssetVisualArtifactSerializer
{
	/** 序列化某资产 N 个 phase 视觉行 + 每 phase 各一张 PNG 到 payload 二进制。
	 * @param Entries           Phase 视觉行数组；要求长度 >= 1，所有元素的 AssetPath/ShardId/ProviderId/EmbeddingDim
	 *                          必须一致（单个资产内的多 phase）。
	 * @param PerPhasePreviewPngs 每 phase 一份 PNG 字节；长度必须等于 Entries。
	 *                          某 phase 不携带 preview 时该项为空数组（写出 PreviewPngByteCount=0）。
	 * @param OutBytes          输出二进制 payload；append 在已有内容后面。 */
	void SerializePayload(
		const TArray<FIndexedAssetVisualEntry>& Entries,
		const TArray<TArray<uint8>>& PerPhasePreviewPngs,
		TArray<uint8>& OutBytes);

	/** 从 payload 反序列化 N 个 phase 视觉行 + 每 phase PNG 字节。
	 * @return true 表示解析成功；false 表示数据损坏或 schema 版本不识别。 */
	bool DeserializePayload(
		const TArray<uint8>& Bytes,
		TArray<FIndexedAssetVisualEntry>& OutEntries,
		TArray<TArray<uint8>>& OutPerPhasePreviewPngs);
}
