#pragma once

#include "CoreMinimal.h"

/*
 * breaker 可以把它理解成“远端缓存的保险丝”。
 *
 * 如果远端 DDC 最近一直失败，我们就先把远端访问关一小会儿，
 * 避免每次都去做注定会失败的慢请求。
 *
 * 之后再放行少量 probe：
 * - get probe 成功了；
 * - put probe 也成功了；
 * 才说明远端真的恢复，可以重新完全打开。
 */
class FMonolithArtifactCacheBreaker
{
public:
	/** 构造时允许自定义打开时长和失败阈值。 */
	explicit FMonolithArtifactCacheBreaker(
		double InOpenDurationSeconds = 60.0,
		int32 InConsecutiveFailureThreshold = 5,
		int32 InRollingFailureThreshold = 8,
		double InRollingWindowSeconds = 30.0);

	/** 清空所有失败历史，回到“完全关闭 breaker”的初始状态。 */
	void Reset();

	/** breaker 当前是否处于打开状态。 */
	bool IsOpen(double NowSeconds) const;
	/** 如果还开着，还剩多少秒会自动放行 probe。 */
	double GetRemainingOpenSeconds(double NowSeconds) const;

	/** 当前是否允许做一次远端 Get。 */
	bool AllowGet(double NowSeconds);
	/** 当前是否允许做一次远端 Put。 */
	bool AllowPut(double NowSeconds);

	/** 记录一次远端 Get 成功。 */
	void RecordRemoteGetSuccess(double NowSeconds);
	/** 记录一次远端 Put 成功。 */
	void RecordRemotePutSuccess(double NowSeconds);
	/** 记录一次远端失败。 */
	void RecordFailure(double NowSeconds);

private:
	/** 正式把 breaker 打开。 */
	void Open(double NowSeconds);
	/** 清理滚动窗口之外的老失败记录。 */
	void PruneFailures(double NowSeconds);
	/** 如果 probe 足够成功，就尝试完全关闭 breaker。 */
	void TryCloseAfterSuccessfulProbes(double NowSeconds);

	/** breaker 每次打开持续多久。 */
	double OpenDurationSeconds = 60.0;
	/** 统计滚动失败数时看的时间窗口大小。 */
	double RollingWindowSeconds = 30.0;
	/** 连续失败多少次就打开 breaker。 */
	int32 ConsecutiveFailureThreshold = 5;
	/** 滚动窗口里累计失败多少次就打开 breaker。 */
	int32 RollingFailureThreshold = 8;

	/** 当前已经连续失败了多少次。 */
	int32 ConsecutiveFailures = 0;
	/** breaker 打开到哪个时刻为止。 */
	double OpenUntilSeconds = 0.0;
	/** 滚动窗口里所有失败发生的时间戳。 */
	TArray<double> FailureTimestamps;
	/** 这一轮恢复探测里，Get probe 是否已经放行过。 */
	bool bGetProbeGranted = false;
	/** 这一轮恢复探测里，Put probe 是否已经放行过。 */
	bool bPutProbeGranted = false;
	/** 这轮 Get probe 是否已经成功。 */
	bool bGetProbeSucceeded = false;
	/** 这轮 Put probe 是否已经成功。 */
	bool bPutProbeSucceeded = false;
};
