#pragma once

#include "CoreMinimal.h"
#include "MonolithArtifactCache.h"

/** warmup 历史中的一条运行记录。 */
struct FMonolithWarmupRunRecord
{
	/** 用于分桶统计的稳定 key，例如 OfflineOnly 或 Cohort:Blueprint。 */
	FString ScopeKey;
	/** 给日志和 UI 展示的范围名字。 */
	FString ScopeDisplay;
	/** 本次运行开始时间，UTC 字符串。 */
	FString StartedAtUtc;
	/** 试图 warmup 多少个包。 */
	int32 AttemptedPackages = 0;
	/** 实际成功 warmup 多少个包。 */
	int32 WarmedPackages = 0;
	/** 这次运行的缓存命中率百分比。 */
	double CacheHitRatePercent = 0.0;
	/** 本地命中次数。 */
	uint64 LocalHitCount = 0;
	/** 远端命中次数。 */
	uint64 RemoteHitCount = 0;
	/** 远端 miss 次数。 */
	uint64 RemoteMissCount = 0;
	/** 远端写入成功次数。 */
	uint64 RemoteWriteOkCount = 0;
	/** 远端写入失败次数。 */
	uint64 RemoteWriteFailCount = 0;
	/** 是否达到了 release gate 阈值。 */
	bool bThresholdMet = false;
};

/** 默认 warmup 历史文件路径。 */
FString GetMonolithWarmupHistoryPath();
/** 从指定路径读取全部历史。 */
bool LoadMonolithWarmupHistory(const FString& FilePath, TArray<FMonolithWarmupRunRecord>& OutRuns);
/** 把全部历史写回指定路径。 */
bool SaveMonolithWarmupHistory(const FString& FilePath, const TArray<FMonolithWarmupRunRecord>& Runs);
/** 在历史尾部追加一条记录，并按最大条数截断。 */
bool AppendMonolithWarmupHistoryRecord(const FString& FilePath, const FMonolithWarmupRunRecord& Run, int32 MaxEntries = 64);
/** 根据本地/远端命中数和 attempted 包数计算命中率。 */
double ComputeMonolithWarmupHitRatePercent(const FMonolithArtifactCacheStats& Stats, int32 AttemptedPackages);
/** 把 release gate 阈值收口到唯一的 0-100 百分比语义。 */
int32 NormalizeMonolithWarmupReleaseThresholdPercent(int32 ThresholdPercent);
/** 统计某个 scope 最近连续多少次达到了阈值。 */
int32 CountConsecutiveThresholdPassingWarmupRuns(
	const TArray<FMonolithWarmupRunRecord>& Runs,
	const FString& ScopeKey,
	double ThresholdPercent);
