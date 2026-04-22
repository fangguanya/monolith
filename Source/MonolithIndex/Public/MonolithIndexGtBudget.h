#pragma once

#include "CoreMinimal.h"

/** 给状态栏/统计面板看的 GT 预算快照。 */
struct FMonolithIndexGtBudgetSnapshot
{
	/** 超预算次数。 */
	uint64 OverrunCount = 0;
	/** 被降级避免 GT load 的次数。 */
	uint64 DowngradeCount = 0;
	/** breaker 当前是否打开。 */
	bool bBreakerOpen = false;
	/** breaker 还会开多久。 */
	double BreakerRemainingSeconds = 0.0;
};

/*
 * GT budget state 的任务是盯住：
 * “哪些 indexer 把游戏线程卡太久了？”
 *
 * 一旦某个 indexer 连续多次超预算，就会：
 * - 把它记进 quarantined 列表；
 * - 必要时打开一个总 breaker，让系统暂时更保守地避开 GT load。
 */
class MONOLITHINDEX_API FMonolithIndexGtBudgetState
{
public:
	/** 重置所有统计和隔离状态。 */
	void Reset();
	/** 记录一次 GT 样本。 */
	void RecordSample(FName IndexerId, double DurationSeconds, double NowSeconds);
	/** 读取当前快照。 */
	FMonolithIndexGtBudgetSnapshot Snapshot(double NowSeconds) const;
	/** 当前是否应当避免对该 indexer 做 GT load。 */
	bool ShouldAvoidGameThreadLoad(FName IndexerId, double NowSeconds) const;

private:
	/** 单次样本超过这个值，就记一次 overrun。 */
	static constexpr double OverrunThresholdSeconds = 0.1;
	/** 连续超多少次就打开总 breaker。 */
	static constexpr int32 ConsecutiveOverrunThreshold = 3;
	/** breaker 打开后持续多久。 */
	static constexpr double BreakerOpenSeconds = 30.0;

	/** 保护内部统计。 */
	mutable FCriticalSection Mutex;
	/** 被列入“尽量别再走 GT load”的 indexer 集合。 */
	TSet<FName> QuarantinedIndexers;
	/** 超预算总次数。 */
	uint64 OverrunCount = 0;
	/** 因隔离而发生的降级总次数。 */
	uint64 DowngradeCount = 0;
	/** 当前已经连续超预算多少次。 */
	int32 ConsecutiveOverruns = 0;
	/** breaker 打开到哪个时刻。 */
	double BreakerOpenUntilSeconds = 0.0;
};
