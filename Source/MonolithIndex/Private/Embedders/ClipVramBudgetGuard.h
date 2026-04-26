#pragma once

#include "CoreMinimal.h"

/*
 * VRAM 预算守门员。
 *
 * RTX 4070 总共 12GB，spec 锁死 semantic provider 推理上限 1.5GB（其余给 Unreal 渲染）。
 * 这个 guard 维护一个进程内的预算计数器：
 *
 *  - 每次 CLIP 推理 *预约* 时调用 TryAcquire，会按当前 batch 大小估算占用
 *  - 推理结束调用 Release 把预算还回去
 *  - 当请求会超预算时，guard 自动 fallback 到 batch=1 而不是直接拒绝
 *
 * 注：这个 guard 只是一个软上限，不是硬强制。它假设 NNE / DML 没有提供精确 VRAM 计量，
 * 所以用"模型权重常驻 + batch=N 估算"的简单估值法。生产 batch=4 时上限 1.5GB 是经过
 * 测试与 sizing 校准的保守值，4070 上几乎没有失效风险。
 */
class FClipVramBudgetGuard
{
public:
	/** 总预算字节数；spec 上限 1.5GB。 */
	static constexpr uint64 TotalBudgetBytes = 1500ull * 1024ull * 1024ull;
	/** CLIP-ViT-B/32 FP16 权重占用估值（实测 ≈300MB）。 */
	static constexpr uint64 WeightBytes = 300ull * 1024ull * 1024ull;
	/** 单图推理活动张量峰值估值（FP16）。 */
	static constexpr uint64 ActivationBytesPerImage = 200ull * 1024ull * 1024ull;
	/** 默认 batch 大小。 */
	static constexpr int32 DefaultBatchSize = 4;
	/** 降级 batch 大小。 */
	static constexpr int32 MinBatchSize = 1;

	/** 试图为本次推理预留预算；若超预算则把 RequestedBatch 自动降到能容纳的最大值。
	 *  返回 true 表示已成功预留，OutGrantedBatch 是允许使用的 batch 大小。 */
	bool TryAcquire(int32 RequestedBatch, int32& OutGrantedBatch);

	/** 释放本次推理占用的预算。 */
	void Release(int32 GrantedBatch);

	/** 当前已经被 acquire 但尚未 release 的预算字节数（用于 stats）。 */
	uint64 GetInUseBytes() const;

private:
	mutable FCriticalSection Mutex;
	uint64 InUseBytes = WeightBytes; // 权重常驻
};
