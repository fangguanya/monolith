#pragma once

#include "CoreMinimal.h"
#include "MonolithArtifactTypes.h"

/** warmup 要跑哪一类工作。 */
enum class EMonolithWarmupScopeKind : uint8
{
	/** 只跑离线型 cohort。 */
	OfflineOnly,
	/** 只跑单个 cohort。 */
	Cohort,
	/** 能跑的都跑。 */
	All,
	/** 只跑需要全局 reducer 的 cohort。 */
	GlobalReducer,
};

/** warmup 的范围参数。 */
struct FMonolithWarmupScope
{
	/** 范围类型。 */
	EMonolithWarmupScopeKind Kind = EMonolithWarmupScopeKind::OfflineOnly;
	/** 当 Kind=Cohort 时，需要知道具体是哪一个 cohort。 */
	FName CohortName;
};

/** 从命令行解析出来的 warmup 参数集合。 */
struct FMonolithWarmupCommandletArgs
{
	/** 本次 warmup 的目标范围。 */
	FMonolithWarmupScope Scope;
	/** 优先级文字，例如 Background。 */
	FString Priority = TEXT("Background");
	/** 最多跑多少分钟；0 表示不限制。 */
	int32 TimeWindowMinutes = 0;
	/** 最多尝试多少个包；0 表示不限制。 */
	int32 MaxPackages = 0;
	/** Horde 分布式切片：本 agent 负责的 shard 哈希区间起点（含），范围 `[0, ShardRangeEnd)`。
	 * 与 ShardRangeEnd 配合使用；二者都为 0 表示不切片，全 shard 都消费。 */
	int32 ShardRangeBegin = 0;
	/** shard 哈希区间终点（不含）；ShardRangeBegin == ShardRangeEnd == 0 表示不切片。 */
	int32 ShardRangeEnd = 0;

	/** 判断当前是否已经跑满了时间窗口。 */
	bool ShouldStopForTimeWindow(double StartSeconds, double NowSeconds) const;
	/** 判断给定 shard id 是否应当被本 agent 消费（按哈希落在 [Begin, End) 内）。 */
	bool IsShardInRange(const FString& ShardId) const;
};

/** 把 `-Scope=... -Priority=...` 这样的命令行解析成结构体。 */
MONOLITHINDEX_API bool ParseMonolithWarmupCommandletArgs(
	const FString& Params,
	FMonolithWarmupCommandletArgs& OutArgs,
	FString& OutError);
/** 判断某个 warmup scope 是否显式命中了指定 indexer。
 * 这里统一比较 indexer id 和显示名，避免各处重复写 cohort 匹配规则。 */
MONOLITHINDEX_API bool DoesMonolithWarmupScopeTargetIndexer(
	const FMonolithWarmupScope& Scope,
	const FName& IndexerId,
	const FString& IndexerName,
	EMonolithExecutionMode ExecutionMode);

/** 从完整命令行里提取 `run=` 后面的 commandlet 名。 */
MONOLITHINDEX_API FString GetRunningMonolithCommandletNameFromCommandLine(const FString& CommandLine);
/** 判断某些 Monolith commandlet 是否需要绕过本地 SQLite 的常规打开路径。 */
MONOLITHINDEX_API bool ShouldMonolithCommandletBypassLocalSqlite(const FString& CommandletName);
