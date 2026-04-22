#include "MonolithArtifactCacheBreaker.h"

/*
 * 这个实现文件只做一件事：
 * 用一套非常简单、容易解释的规则，判断“远端缓存现在是不是值得继续尝试”。
 */

FMonolithArtifactCacheBreaker::FMonolithArtifactCacheBreaker(
	const double InOpenDurationSeconds,
	const int32 InConsecutiveFailureThreshold,
	const int32 InRollingFailureThreshold,
	const double InRollingWindowSeconds)
	: OpenDurationSeconds(InOpenDurationSeconds)
	, RollingWindowSeconds(InRollingWindowSeconds)
	, ConsecutiveFailureThreshold(InConsecutiveFailureThreshold)
	, RollingFailureThreshold(InRollingFailureThreshold)
{
}

void FMonolithArtifactCacheBreaker::Reset()
{
	// Reset 代表完全忘记之前的失败历史，重新开始观察。
	ConsecutiveFailures = 0;
	OpenUntilSeconds = 0.0;
	FailureTimestamps.Reset();
	bGetProbeGranted = false;
	bPutProbeGranted = false;
	bGetProbeSucceeded = false;
	bPutProbeSucceeded = false;
}

bool FMonolithArtifactCacheBreaker::IsOpen(const double NowSeconds) const
{
	return OpenUntilSeconds > NowSeconds;
}

double FMonolithArtifactCacheBreaker::GetRemainingOpenSeconds(const double NowSeconds) const
{
	return FMath::Max(0.0, OpenUntilSeconds - NowSeconds);
}

bool FMonolithArtifactCacheBreaker::AllowGet(const double NowSeconds)
{
	if (IsOpen(NowSeconds))
	{
		// breaker 还开着时，连 probe 都不允许。
		return false;
	}

	if (OpenUntilSeconds <= 0.0)
	{
		// 从未打开过 breaker，或者已经完全恢复，允许正常访问。
		return true;
	}

	if (!bGetProbeGranted)
	{
		// breaker 刚从打开状态走出来时，只放行一次 get probe。
		bGetProbeGranted = true;
		return true;
	}

	return false;
}

bool FMonolithArtifactCacheBreaker::AllowPut(const double NowSeconds)
{
	if (IsOpen(NowSeconds))
	{
		return false;
	}

	if (OpenUntilSeconds <= 0.0)
	{
		return true;
	}

	if (!bPutProbeGranted)
	{
		// put 方向也只先给一次恢复探测机会。
		bPutProbeGranted = true;
		return true;
	}

	return false;
}

void FMonolithArtifactCacheBreaker::RecordRemoteGetSuccess(const double NowSeconds)
{
	ConsecutiveFailures = 0;
	if (OpenUntilSeconds > 0.0)
	{
		// 只有 breaker 曾经开过时，这次成功才算“恢复探测成功”。
		bGetProbeSucceeded = true;
		TryCloseAfterSuccessfulProbes(NowSeconds);
	}
}

void FMonolithArtifactCacheBreaker::RecordRemotePutSuccess(const double NowSeconds)
{
	ConsecutiveFailures = 0;
	if (OpenUntilSeconds > 0.0)
	{
		bPutProbeSucceeded = true;
		TryCloseAfterSuccessfulProbes(NowSeconds);
	}
}

void FMonolithArtifactCacheBreaker::RecordFailure(const double NowSeconds)
{
	// 每次失败都同时记入“连续失败”和“滚动窗口失败”两套统计。
	++ConsecutiveFailures;
	FailureTimestamps.Add(NowSeconds);
	PruneFailures(NowSeconds);

	if (ConsecutiveFailures >= ConsecutiveFailureThreshold || FailureTimestamps.Num() >= RollingFailureThreshold)
	{
		Open(NowSeconds);
	}
}

void FMonolithArtifactCacheBreaker::Open(const double NowSeconds)
{
	// 打开 breaker 后，之前探测结果全部失效，要重新来过。
	OpenUntilSeconds = NowSeconds + OpenDurationSeconds;
	ConsecutiveFailures = 0;
	bGetProbeGranted = false;
	bPutProbeGranted = false;
	bGetProbeSucceeded = false;
	bPutProbeSucceeded = false;
}

void FMonolithArtifactCacheBreaker::PruneFailures(const double NowSeconds)
{
	// 只保留滚动窗口内的失败记录。
	FailureTimestamps.RemoveAll([this, NowSeconds](const double Timestamp)
	{
		return (NowSeconds - Timestamp) > RollingWindowSeconds;
	});
}

void FMonolithArtifactCacheBreaker::TryCloseAfterSuccessfulProbes(const double NowSeconds)
{
	if (bGetProbeSucceeded && bPutProbeSucceeded)
	{
		// get/put 两边都恢复，breaker 才算真正关闭。
		OpenUntilSeconds = 0.0;
		FailureTimestamps.Reset();
		bGetProbeGranted = false;
		bPutProbeGranted = false;
		bGetProbeSucceeded = false;
		bPutProbeSucceeded = false;
		ConsecutiveFailures = 0;
	}
	else
	{
		// 只成功了一半时，先把“打开到未来某时刻”的状态改成“允许继续下一次 probe”。
		OpenUntilSeconds = NowSeconds;
	}
}
