#include "Embedders/ClipVramBudgetGuard.h"

#include "MonolithIndexLog.h"

bool FClipVramBudgetGuard::TryAcquire(const int32 RequestedBatch, int32& OutGrantedBatch)
{
	const int32 ClampedBatch = FMath::Clamp(RequestedBatch, MinBatchSize, DefaultBatchSize);

	FScopeLock Lock(&Mutex);

	// 优先尝试 ClampedBatch；不行就一路降到 MinBatchSize。
	for (int32 Try = ClampedBatch; Try >= MinBatchSize; --Try)
	{
		const uint64 NeededActivation = ActivationBytesPerImage * static_cast<uint64>(Try);
		const uint64 NeededTotal = WeightBytes + NeededActivation;
		if (NeededTotal <= TotalBudgetBytes && (InUseBytes + NeededActivation) <= TotalBudgetBytes)
		{
			InUseBytes += NeededActivation;
			OutGrantedBatch = Try;
			return true;
		}
	}

	OutGrantedBatch = 0;
	UE_LOG(LogMonolithIndex, Warning,
		TEXT("ClipVramBudgetGuard: 已无法在 1.5GB 预算内预留任何 batch（当前占用 %llu MB）"),
		InUseBytes / (1024ull * 1024ull));
	return false;
}

void FClipVramBudgetGuard::Release(const int32 GrantedBatch)
{
	if (GrantedBatch <= 0)
	{
		return;
	}
	FScopeLock Lock(&Mutex);
	const uint64 Reclaim = ActivationBytesPerImage * static_cast<uint64>(GrantedBatch);
	if (InUseBytes >= WeightBytes + Reclaim)
	{
		InUseBytes -= Reclaim;
	}
	else
	{
		// 只把"权重常驻 + 0 活动张量"作为最低占用，避免负值。
		InUseBytes = WeightBytes;
	}
}

uint64 FClipVramBudgetGuard::GetInUseBytes() const
{
	FScopeLock Lock(&Mutex);
	return InUseBytes;
}
