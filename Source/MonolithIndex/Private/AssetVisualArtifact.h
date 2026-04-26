#pragma once

#include "CoreMinimal.h"
#include "AssetVisualEntry.h"

/*
 * AssetVisual artifact 内部序列化协议（geometric / semantic 共用）。
 *
 * 文件级 layout：
 *   uint8  PayloadSchema = 1
 *   string AssetPath
 *   string ShardId
 *   uint32 ShardPrefixDepth
 *   string ProviderId
 *   uint32 ProviderVersion
 *   uint32 RenderRecipeVersion
 *   uint32 EmbeddingDim
 *   uint8  EmbeddingDtype       // 0=FP32, 1=FP16
 *   uint32 EmbeddingByteCount
 *   bytes  EmbeddingBytes
 *   uint32 PreviewPngByteCount  // 0 表示该 artifact 不携带 preview（双 cohort 共享时只有 owner 携带）
 *   bytes  PreviewPngBytes
 *
 * 写入约定：
 *  - geometric cohort 是 preview PNG 的 owner（先索引），artifact 携带 PNG 字节
 *  - semantic cohort 复用 geometric 写下的 preview PNG，artifact 内 PreviewPngByteCount=0
 *
 * 这样既不需要额外的"共享文件 store"，也保证两个 cohort 的 artifact 完全可独立 round-trip。
 */
namespace AssetVisualArtifactSerializer
{
	/** 序列化单 mesh 视觉行 + 可选 PNG 字节到 payload 二进制；
	 * @param Entry        视觉行；必须填好 ProviderId / EmbeddingBytes 等
	 * @param PreviewPng   可选 preview PNG 字节；空表示该 artifact 不携带 preview
	 * @param OutBytes     输出二进制 payload；append 在已有内容后面 */
	void SerializePayload(
		const FIndexedAssetVisualEntry& Entry,
		const TArray<uint8>& PreviewPng,
		TArray<uint8>& OutBytes);

	/** 从 payload 反序列化视觉行 + 可选 PNG 字节。
	 * @return true 表示解析成功；false 表示数据损坏或 schema 版本不识别 */
	bool DeserializePayload(
		const TArray<uint8>& Bytes,
		FIndexedAssetVisualEntry& OutEntry,
		TArray<uint8>& OutPreviewPng);
}
