#pragma once

#include "AssetVisualEmbeddingProvider.h"

/*
 * Geometric embedding provider: provider_id = `geometric_v1`，64 维 FP32 向量。
 *
 * 设计原则：
 *  - 索引路径与查询路径走完全相同的 Encode()，保证 cosine 比较在同一向量空间里完成；
 *  - 输入只需要 1 张 iso color view + 1 张同分辨率单通道 silhouette mask；
 *  - 没有任何随机源，没有任何外部依赖（不 NNE / 不 OpenCV）；
 *  - 同一对 (color, silhouette) 输入必产 bit-identical 输出。
 *
 * 64 维结构（向量内偏移已锁定，禁止重排）：
 *   [0..6]    : 全 silhouette 7 维 Hu moments (log-scaled，符号保留)
 *   [7..13]   : 左半 silhouette Hu moments
 *   [14..20]  : 右半 silhouette Hu moments
 *   [21..27]  : 上半 silhouette Hu moments
 *   [28..34]  : 下半 silhouette Hu moments
 *   [35]      : silhouette bbox log(W/H)
 *   [36]      : silhouette 占图像总面积比例
 *   [37]      : silhouette 重心 X 偏离图像中心（归一化到 [-1,1]）
 *   [38]      : silhouette 重心 Y 偏离图像中心（归一化到 [-1,1]）
 *   [39]      : silhouette bbox 归一化宽
 *   [40]      : silhouette bbox 归一化高
 *   [41]      : silhouette 紧凑度（4πA / P²，圆=1）
 *   [42]      : silhouette 周长 / 图像对角线（归一化）
 *   [43]      : 前景区域平均 L*（CIE Lab L 分量，归一化到 [0,1]）
 *   [44]      : 前景区域 L* 标准差
 *   [45..63]  : 颜色 perceptual hash 19 维（低频 DCT 投影到 [-1,1]）
 *
 * 任何会改变上述偏移含义、维度数、特征算法的代码改动都必须 bump ProviderVersion，
 * 触发整 cohort 重新索引；spec / design 与本文件偏移表必须保持一致。
 */
class FGeometricEmbeddingProvider final : public IAssetVisualEmbeddingProvider
{
public:
	FGeometricEmbeddingProvider();

	virtual FAssetVisualProviderInfo GetProviderInfo() const override { return Info; }

	/** geometric provider 不依赖外部资源，永远可用。 */
	virtual bool IsAvailable() const override { return true; }

	/**
	 * 把 1 张 iso color + 1 张同分辨率 silhouette 编码成 64 维 L2-normalized 向量。
	 *
	 * @param ColorImage      BGRA8 sRGB 输入；任意分辨率
	 * @param SilhouetteImage 单通道 G8 mask（非 0 像素=前景）；分辨率必须等于 ColorImage
	 * @param OutEmbedding    输出 64 维向量
	 *
	 * @return 编码成功返回 true；输入分辨率不匹配 / 全空 silhouette 视情况返回 false
	 */
	virtual bool Encode(
		const FImage& ColorImage,
		const FImage& SilhouetteImage,
		TArray<float>& OutEmbedding) override;

private:
	FAssetVisualProviderInfo Info;
};
