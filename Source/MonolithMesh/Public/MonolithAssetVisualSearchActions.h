#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/*
 * asset.search_assets_by_image action 注册入口。
 *
 * 这是 AssetVisual 双 cohort 的查询前端：
 *  - 接受 image_path / image_base64 + 可选 bbox / mask + 可选 category_hint / size_hint
 *  - 并行查询 AssetVisualGeometric + AssetVisualSemantic（默认 provider=auto）
 *  - 通过 FAssetVisualShardedRetriever 跨 shard 并行 brute-force cosine 召回
 *  - 二阶段 rerank 结合 MeshCatalog 的尺寸 / 长宽比 / 类别 / 三角面
 *  - 召回范围覆盖 4 类资产：StaticMesh / SkeletalMesh / Material / WidgetBlueprint
 *
 * 安全 / 性能边界：
 *  - 图片输入解码后长边 ≤4096 否则降采样到 ≤1024
 *  - 仅支持 PNG / JPEG / WebP；image_base64 ≤ 8MB
 *  - image_path 必须在项目 Saved/ 或 Intermediate/Capture/ 下；拒绝路径越界
 *  - bbox 像素 [x,y,w,h]；mask 单通道或等尺寸 PNG
 *  - top-K 默认 10，clamp 到 [1, 100]
 *  - 必须运行在 BackgroundCpuPool（注册时 ExecutionPolicy=BackgroundThread）
 */
class FMonolithAssetVisualSearchActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleSearchAssetsByImage(const TSharedPtr<FJsonObject>& Params);
};
