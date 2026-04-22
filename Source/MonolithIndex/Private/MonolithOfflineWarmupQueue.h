#pragma once

#include "CoreMinimal.h"

/** 离线 warmup 队列中的一条请求。 */
struct FMonolithOfflineWarmupRequest
{
	/** 要 warmup 的包路径。 */
	FString PackagePath;
	/** 资产类名，仅用于日志/调试展示。 */
	FString AssetClass;
	/** 负责它的 indexer。 */
	FString IndexerId;
	/** 为什么被加入队列。 */
	FString Reason;
	/** 入队时间，UTC 字符串格式。 */
	FString EnqueuedAtUtc;

	/** 两条请求是否指向同一个“目标资产 + indexer”组合。 */
	bool MatchesTarget(const FMonolithOfflineWarmupRequest& Other) const
	{
		return PackagePath.Equals(Other.PackagePath, ESearchCase::CaseSensitive)
			&& IndexerId.Equals(Other.IndexerId, ESearchCase::CaseSensitive);
	}
};

/** 获取默认的离线 warmup 队列文件路径。 */
FString GetMonolithOfflineWarmupQueuePath();

/** 从指定路径读取整份队列。 */
bool LoadMonolithOfflineWarmupQueue(const FString& FilePath, TArray<FMonolithOfflineWarmupRequest>& OutRequests);
/** 把整份队列写回指定路径。 */
bool SaveMonolithOfflineWarmupQueue(const FString& FilePath, const TArray<FMonolithOfflineWarmupRequest>& Requests);
/** 向指定路径的队列里添加或更新一条请求。 */
bool EnqueueMonolithOfflineWarmupRequest(const FString& FilePath, const FMonolithOfflineWarmupRequest& Request);
/** 从指定路径的队列里移除一批已完成请求。 */
int32 RemoveMonolithOfflineWarmupRequests(const FString& FilePath, const TArray<FMonolithOfflineWarmupRequest>& CompletedRequests);

/** 读取默认路径的队列。 */
bool LoadMonolithOfflineWarmupQueue(TArray<FMonolithOfflineWarmupRequest>& OutRequests);
/** 写回默认路径的队列。 */
bool SaveMonolithOfflineWarmupQueue(const TArray<FMonolithOfflineWarmupRequest>& Requests);
/** 向默认路径的队列入队。 */
bool EnqueueMonolithOfflineWarmupRequest(const FMonolithOfflineWarmupRequest& Request);
/** 从默认路径的队列移除已完成请求。 */
int32 RemoveMonolithOfflineWarmupRequests(const TArray<FMonolithOfflineWarmupRequest>& CompletedRequests);

/** 某个包当前是否已经在离线 warmup 队列里。 */
bool IsPackageQueuedForMonolithOfflineWarmup(const FString& PackagePath);
/** 把所有已排队包路径附加到外部集合里。 */
void AppendMonolithOfflineWarmupQueuedPackages(TSet<FString>& OutPackages);
