#pragma once

#include "AssetVisualEmbeddingProvider.h"

class FClipVramBudgetGuard;
class FClipPieSuspender;

/*
 * Semantic embedding provider: provider_id = `clip_vit_b32_v1`，512 维 FP16 向量。
 *
 * 通过 UE 5.7 内置 NNE + ONNXRuntime 调用 CLIP-ViT-B/32 image encoder。
 *
 * 关键约束：
 *  - 模型路径写死：Plugins/Monolith/Resources/Models/clip_vit_b32_image_encoder_fp16.onnx
 *  - ProviderVersion 与 ONNX 文件 Blake3 哈希绑定（任一字节变化触发 cohort 全量 stale）
 *  - VRAM 预算 1.5GB；超预算时 batch 自动降到 1
 *  - PIE 期间整个 provider IsAvailable() 临时返回 false（保持 cohort stale 直到 PIE 退出）
 *  - NNE 不可用 / ONNX 文件缺失时 IsAvailable() 永久返回 false（geometric cohort 不受影响）
 *
 * 编译期通过 `WITH_MONOLITH_NNE` 宏分流：
 *  - WITH_MONOLITH_NNE=1：完整接通 NNERuntimeORT 推理路径
 *  - WITH_MONOLITH_NNE=0：所有方法存在但 IsAvailable=false，Encode 直接返回 false
 */
class FClipSemanticEmbeddingProvider final : public IAssetVisualEmbeddingProvider
{
public:
	FClipSemanticEmbeddingProvider();
	virtual ~FClipSemanticEmbeddingProvider() override;

	virtual FAssetVisualProviderInfo GetProviderInfo() const override { return Info; }
	virtual bool IsAvailable() const override;

	virtual bool Encode(
		const FImage& ColorImage,
		const FImage& SilhouetteImage,
		TArray<float>& OutEmbedding) override;

private:
	/** 计算 ONNX 文件路径并加载模型；同时把 ProviderVersion 锁定到 Blake3(model_bytes) 的低 32 位。 */
	void LoadModelIfNeeded();

	/** 把 BGRA8 sRGB 图像 resize 到 224×224、转 BGR float、按 CLIP mean/std 归一化。 */
	bool BuildClipInputTensor(const FImage& ColorImage, TArray<float>& OutNCHW) const;

	FAssetVisualProviderInfo Info;
	mutable FCriticalSection LoadMutex;
	bool bLoadAttempted = false;
	bool bLoadSucceeded = false;
	FString OnnxModelPath;

	TUniquePtr<FClipVramBudgetGuard> BudgetGuard;
	TUniquePtr<FClipPieSuspender> PieSuspender;

	/** NNE 句柄打包到 PIMPL，避免 NNE 头文件污染 .h；定义见 .cpp 内 #if WITH_MONOLITH_NNE 段。 */
	struct FNneState;
	TUniquePtr<FNneState> NneState;
};
