#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** 给 UI 和查询侧看的运行时快照。 */
struct FMonolithIndexRuntimeSnapshot
{
	/** 当前是否还有索引任务在跑。 */
	bool bIndexingInProgress = false;
	/** 0 到 1 之间的总体进度。 */
	double Progress = 0.0;
	/** 估算剩余秒数。 */
	double EtaSeconds = 0.0;
	/** 已完成项数。 */
	int32 CompletedItems = 0;
	/** 总项数。 */
	int32 TotalItems = 0;
	/** 当前队列深度。 */
	int32 QueueDepth = 0;
	/** 剩余项数。 */
	int32 RemainingItems = 0;
	/** stale 包总数。 */
	int32 StalePackageCount = 0;
};

/*
 * runtime state 是 MonolithIndex 的“记分牌”。
 *
 * 它不负责真正索引，而是负责记录：
 * - 现在是不是在索引；
 * - 已经做了多少；
 * - 还剩多少；
 * - 哪些包是 stale。
 */
class FMonolithIndexRuntimeState
{
public:
	/** 清空整块运行时状态。 */
	void Reset();
	/** 开始一轮新的索引会话。 */
	void BeginSession(const TSet<FString>& InStalePackages, int32 InTotalProgressItems);
	/** 更新“主进度条”里的已完成/总量。 */
	void UpdateProgress(int32 InCurrentProgressItems, int32 InTotalProgressItems);
	/** 开始一些还没法精确映射到具体包的匿名工作。 */
	void BeginAnonymousWork(int32 Count = 1);
	/** 完成一部分匿名工作。 */
	void CompleteAnonymousWork(int32 Count = 1);
	/** 结束当前会话。 */
	void FinishSession();

	/** 某个包当前是否被标记为 stale。 */
	bool IsPackageStale(const FString& PackagePath) const;
	/** 把 stale 包集合附加到外部集合里。 */
	void AppendStalePackages(TSet<FString>& OutPackages) const;
	/** 生成一次线程安全的快照。 */
	FMonolithIndexRuntimeSnapshot Snapshot() const;
	/** 生成分页版 stale 包列表。 */
	TSharedPtr<FJsonObject> BuildStalePackagesPage(int32 Limit, const FString& Cursor) const;
	/** 把任意一组包路径按统一 cursor 协议分页成 JSON。
	 * 这样 runtime state、subsystem、action 就不会再各自复制一份分页协议。 */
	static TSharedPtr<FJsonObject> BuildPackagePage(const TSet<FString>& Packages, int32 Limit, const FString& Cursor);
	/** 把分页偏移量编码成游标字符串。 */
	static FString EncodeCursor(int32 Offset);
	/** 从游标字符串解析出偏移量。 */
	static int32 DecodeCursor(const FString& Cursor);

	/** 规范化路径前缀，方便做批量 stale 过滤。 */
	static TArray<FString> NormalizePackagePrefixes(const TArray<FString>& InPrefixes);
	/** 某个包是否命中了任一路径前缀。 */
	static bool PackageMatchesAnyPrefix(const FString& PackagePath, const TArray<FString>& Prefixes);

private:
	/** 保护下面所有运行时字段。 */
	mutable FCriticalSection Mutex;
	/** 当前会话里被认为 stale 的包集合。 */
	TSet<FString> StalePackages;
	/** 主进度条已完成项数。 */
	int32 CurrentProgressItems = 0;
	/** 主进度条总项数。 */
	int32 TotalProgressItems = 0;
	/** 匿名工作总项数。 */
	int32 AnonymousTotalItems = 0;
	/** 尚未完成的匿名工作项数。 */
	int32 AnonymousOutstandingItems = 0;
	/** 本次会话开始的时刻，用来估 ETA。 */
	double SessionStartSeconds = 0.0;
	/** 当前是否有活跃会话。 */
	bool bActive = false;
};
