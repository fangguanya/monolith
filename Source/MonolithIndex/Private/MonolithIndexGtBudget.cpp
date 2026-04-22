#include "MonolithIndexGtBudget.h"

/*
 * GT budget 的目标不是精准做性能分析，
 * 而是尽快发现“这个 indexer 最近老是把 GT 卡住”的趋势，然后保守降级。
 */

void FMonolithIndexGtBudgetState::Reset()
{
	FScopeLock Lock(&Mutex);
	QuarantinedIndexers.Reset();
	OverrunCount = 0;
	DowngradeCount = 0;
	ConsecutiveOverruns = 0;
	BreakerOpenUntilSeconds = 0.0;
}

void FMonolithIndexGtBudgetState::RecordSample(const FName IndexerId, const double DurationSeconds, const double NowSeconds)
{
	FScopeLock Lock(&Mutex);

	if (BreakerOpenUntilSeconds > 0.0 && NowSeconds >= BreakerOpenUntilSeconds)
	{
		// breaker 到时后自动关闭，重新允许观察。
		BreakerOpenUntilSeconds = 0.0;
	}

	if (DurationSeconds <= OverrunThresholdSeconds)
	{
		// 一次“正常样本”会把连续超预算计数清零。
		ConsecutiveOverruns = 0;
		return;
	}

	++OverrunCount;
	++ConsecutiveOverruns;

	if (!IndexerId.IsNone() && !QuarantinedIndexers.Contains(IndexerId))
	{
		// 第一次把某个 indexer 拉黑时，才算一次 downgrade。
		QuarantinedIndexers.Add(IndexerId);
		++DowngradeCount;
	}

	if (ConsecutiveOverruns >= ConsecutiveOverrunThreshold)
	{
		// 整体趋势已经不好，就打开总 breaker 一段时间。
		BreakerOpenUntilSeconds = FMath::Max(BreakerOpenUntilSeconds, NowSeconds + BreakerOpenSeconds);
	}
}

FMonolithIndexGtBudgetSnapshot FMonolithIndexGtBudgetState::Snapshot(const double NowSeconds) const
{
	FScopeLock Lock(&Mutex);

	FMonolithIndexGtBudgetSnapshot Result;
	Result.OverrunCount = OverrunCount;
	Result.DowngradeCount = DowngradeCount;
	if (BreakerOpenUntilSeconds > NowSeconds)
	{
		Result.bBreakerOpen = true;
		Result.BreakerRemainingSeconds = BreakerOpenUntilSeconds - NowSeconds;
	}
	return Result;
}

bool FMonolithIndexGtBudgetState::ShouldAvoidGameThreadLoad(const FName IndexerId, const double NowSeconds) const
{
	FScopeLock Lock(&Mutex);
	// 只要总 breaker 开着，或者该 indexer 已被隔离，就尽量别再走 GT load。
	return (BreakerOpenUntilSeconds > NowSeconds) || (!IndexerId.IsNone() && QuarantinedIndexers.Contains(IndexerId));
}
