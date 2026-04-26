#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"
#include "Templates/SharedPointer.h"

/*
 * AssetVisual embedding provider 抽象。
 *
 * 这份接口的目标是把"如何把 mesh canonical view 编码成定长向量"这件事关在一个明确边界后面：
 *
 *  +-----------------------------+        +------------------------+
 *  | IAssetCanonicalRenderer  | -----> | IAssetVisualEmbeddingProvider |
 *  +-----------------------------+   img  +------------------------+
 *                                                |
 *                                                | float[D]
 *                                                v
 *                                  +-----------------------------+
 *                                  | sharded ANN snapshot        |
 *                                  +-----------------------------+
 *
 * Provider 的职责非常窄：图片进，向量出。
 *
 * 关键约束：
 * - 每个 cohort 必须绑定唯一一个 provider（spec 强制）；不允许同 cohort 混用多 provider。
 * - Provider 必须 deterministic：同一图片同一 provider 版本，输出必须 bit-identical。
 * - Provider 必须显式声明 ProviderId / ProviderVersion，并把它们写进 cohort artifact Meta。
 * - 任何会改变 embedding 数学结果的代码改动都必须 bump ProviderVersion，整 cohort 立刻 stale。
 *
 * v1 提供两个 in-process 实现：
 *  - GeometricEmbeddingProvider (`geometric_v1`)：纯 CPU，64 维 FP32，零外部依赖
 *  - ClipSemanticEmbeddingProvider (`clip_vit_b32_v1`)：NNE + ONNX，512 维 FP16，1.5GB VRAM 上限
 */

/** Provider 元信息：写进 cohort artifact Meta，决定 stale 边界。 */
struct MONOLITHINDEX_API FAssetVisualProviderInfo
{
	/** 稳定唯一的 provider 标识符，例如 `geometric_v1`、`clip_vit_b32_v1`。 */
	FName ProviderId;
	/** Provider 版本号；任何输出语义变化必须 bump，触发 cohort 全量 stale。 */
	uint32 ProviderVersion = 1;
	/** 输出向量维度；同 cohort 内固定，不允许变化。 */
	int32 EmbeddingDim = 0;
	/** 向量元素是否已被 L2 标准化；标准化后召回侧做点积即可，无需再除模长。 */
	bool bL2Normalized = true;
};

/*
 * IAssetVisualEmbeddingProvider 是把 canonical view 编码成 embedding 的统一接口。
 *
 * 调用方约束：
 * - Encode 必须线程安全，可由 BackgroundCpuPool 多线程并发调用；
 * - Encode 失败时返回 false，OutEmbedding 内容不定；
 * - IsAvailable() 必须可在任何时候安全调用，用于运行时降级判定。
 */
class MONOLITHINDEX_API IAssetVisualEmbeddingProvider
{
public:
	virtual ~IAssetVisualEmbeddingProvider() = default;

	/** Provider 元信息；同实例内必须返回完全相同的常量。 */
	virtual FAssetVisualProviderInfo GetProviderInfo() const = 0;

	/** Provider 当前是否可用。
	 * 例如 semantic provider 在 ONNX 模型未加载或 NNE 不可用时返回 false。 */
	virtual bool IsAvailable() const = 0;

	/**
	 * 把单张 canonical view 编码成定长 embedding。
	 *
	 * @param ColorImage    BGRA8 sRGB 输入图像；分辨率必须与 cohort 协议一致（geometric 由
	 *                      provider 自己再缩放，semantic 必须 256×256）
	 * @param SilhouetteImage 单通道 8bit 前景遮罩；geometric provider 必填，semantic 不使用
	 * @param OutEmbedding 输出向量；长度必须等于 GetProviderInfo().EmbeddingDim
	 * @return true 表示编码成功；false 表示失败（外部应把对应 mesh 标 stale）
	 */
	virtual bool Encode(
		const FImage& ColorImage,
		const FImage& SilhouetteImage,
		TArray<float>& OutEmbedding) = 0;
};

/*
 * Provider 注册表：进程内全局单例，承载所有当前活动的 cohort provider。
 *
 * 设计：
 * - 启动期由 MonolithIndexSubsystem 注册各 cohort 的 provider；
 * - 运行期 indexer / retriever 通过 ProviderId 查询；
 * - 不允许同一个 ProviderId 注册两次。
 */
class MONOLITHINDEX_API FAssetVisualEmbeddingProviderRegistry
{
public:
	/** 取全局单例。 */
	static FAssetVisualEmbeddingProviderRegistry& Get();

	/** 注册一个 provider；同 ProviderId 重复注册会触发 ensure。 */
	void RegisterProvider(TSharedPtr<IAssetVisualEmbeddingProvider> Provider);

	/** 反注册；通常在模块关闭或测试 teardown 时使用。 */
	void UnregisterProvider(const FName ProviderId);

	/** 按 ProviderId 查找 provider；找不到返回 nullptr。 */
	TSharedPtr<IAssetVisualEmbeddingProvider> FindProvider(const FName ProviderId) const;

	/** 列出所有当前注册的 provider 元信息（用于 stats / 调试）。 */
	TArray<FAssetVisualProviderInfo> ListProviders() const;

private:
	mutable FCriticalSection RegistryLock;
	TMap<FName, TSharedPtr<IAssetVisualEmbeddingProvider>> Providers;
};

/*
 * 从 BGRA8 sRGB color 输入按"非黑色像素=前景"规则生成单通道 silhouette。
 *
 * 阈值与 IAssetCanonicalRenderer 内部使用的 R+G+B>24 完全一致，
 * 保证 indexer 与查询路径看到的同一图像产出 bit-identical silhouette。
 *
 * 这是一个跨模块共享的小 helper：
 *  - geometric indexer 不需要它（直接拿 renderer 给的 silhouette）；
 *  - asset.search_assets_by_image 必须用它从 query color 推 silhouette。
 *
 * 放在 provider 公共头里避免 MonolithMesh 反向 include MonolithIndex 的 Private 头。
 */
MONOLITHINDEX_API void DeriveAssetVisualSilhouetteFromColor(
	const FImage& ColorImage,
	FImage& OutSilhouette);
