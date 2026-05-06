#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "EditorSubsystem.h"
#include "IO/IoHash.h"
#include "Misc/AsyncTaskNotification.h"
#include "MonolithArtifactCache.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexGtBudget.h"
#include "MonolithIndexer.h"
#include "MonolithIndexSubsystem.generated.h"

struct FMonolithActionResult;

/*
 * UMonolithIndexSubsystem 是 MonolithIndex 的“大总管”。
 *
 * 它负责把几条原本很分散的链路串起来：
 * 1. 启动时打开数据库、注册 indexer、决定是否自动建索引；
 * 2. full index / incremental index / live update 三条写入路径；
 * 3. 查询入口，例如 search / stats / stale packages；
 * 4. 调度器、artifact cache、runtime state、GT 预算这些运行时对象。
 *
 * 可以把它想成一个车站调度台：
 * - Asset Registry 是列车时刻表；
 * - Scheduler 是发车调度员；
 * - Database 是终点站仓库；
 * - 各个 Indexer 是不同线路的检票员。
 *
 * 这个类之所以长，是因为它正站在“资产变化”“后台任务”“前台查询”三者的交叉点上。
 */

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnIndexingProgress, int32 /*Current*/, int32 /*Total*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnIndexingComplete, bool /*bSuccess*/);

class FMonolithIndexRuntimeState;
class FMonolithIndexScheduler;

/** `FMonolithIndexRuntimeState` 是前向声明类型，所以删除动作放到 cpp 再定义。 */
struct FMonolithIndexRuntimeStateDeleter
{
	void operator()(FMonolithIndexRuntimeState* Ptr) const;
};

/** `FMonolithIndexScheduler` 同理，用自定义 deleter 避免头文件里要求完整类型。 */
struct FMonolithIndexSchedulerDeleter
{
	void operator()(FMonolithIndexScheduler* Ptr) const;
};

enum class EIndexChangeType : uint8
{
    /** 新资产出现了。 */
    Added,
    /** 资产被删掉了。 */
    Removed,
    /** 资产改名了。 */
    Renamed,
    /** 资产内容更新了。 */
    Updated
};

struct FPendingIndexChange
{
    /** 这次变化属于哪一种。 */
    EIndexChangeType Type;
    /** Asset Registry 提供的最新资产信息。 */
    FAssetData AssetData;
    /** 改名前的旧对象路径，只有 Rename 事件会填。 */
    FString OldObjectPath;
};

/** 记录一个被纳入索引范围的插件。 */
struct FIndexedPluginInfo
{
    /** 插件逻辑名。 */
    FString PluginName;
    /** Asset Registry 里的挂载根。 */
    FString MountPath;
    /** 磁盘上的 Content 目录。 */
    FString ContentDir;
    /** 给 UI 看的人类友好名称。 */
    FString FriendlyName;
};

struct FMonolithIndexStatusBarSnapshot
{
	/** 数据库是否已打开。 */
	bool bDatabaseOpen = false;
	/** 索引功能是否启用。 */
	bool bIndexEnabled = false;
	/** 本地 artifact cache 是否可用。 */
	bool bLocalCacheAvailable = false;
	/** 当前是否有索引任务正在跑。 */
	bool bIndexingInProgress = false;
	/** 进度百分比，范围 0 到 1。 */
	double Progress = 0.0;
	/** 预计剩余秒数。 */
	double EtaSeconds = 0.0;
	/** 已完成项数量。 */
	int32 CompletedItems = 0;
	/** 总项数。 */
	int32 TotalItems = 0;
	/** 队列深度。 */
	int32 QueueDepth = 0;
	/** 剩余项数量。 */
	int32 RemainingItems = 0;
	/** stale 包数量，未知时用 INDEX_NONE。 */
	int32 StalePackageCount = INDEX_NONE;
	/** 离线 warmup 队列深度。 */
	int32 OfflineQueueDepth = INDEX_NONE;
	/** 本地 artifact 命中次数。 */
	uint64 LocalHitCount = 0;
	/** 远端 artifact 命中次数。 */
	uint64 RemoteHitCount = 0;
	/** 远端 artifact 未命中次数。 */
	uint64 RemoteMissCount = 0;
	/** 远端写入成功次数。 */
	uint64 RemoteWriteOkCount = 0;
	/** 远端写入失败次数。 */
	uint64 RemoteWriteFailCount = 0;
	/** 远端写入字节总量。 */
	uint64 RemoteWriteBytes = 0;
	/** 因 oversized 而跳过共享缓存的 artifact 次数。 */
	uint64 OversizedArtifactCount = 0;
	/** 还排队等着同步到远端 DDC 的请求数。 */
	int32 PendingRemoteWriteCount = 0;
	/** 当前已经发出去、还没完成的远端写请求数。 */
	int32 InFlightRemoteWriteCount = 0;
	/** GT 超预算次数。 */
	uint64 GtOverrunCount = 0;
	/** GT 被迫降级次数。 */
	uint64 GtDowngradeCount = 0;
	/** GT breaker 当前是否打开。 */
	bool bGtBreakerOpen = false;
	/** 远端缓存是否临时禁用。 */
	bool bRemoteDisabled = false;
	/** GT breaker 剩余时间。 */
	double GtBreakerRemainingSeconds = 0.0;
	/** 远端 breaker 剩余时间。 */
	double RemoteBreakerRemainingSeconds = 0.0;
	/** 状态栏最终展示的文字。 */
	FString StatusMessage;
};

/*
 * 这是编辑器里的 MonolithIndex 子系统声明。
 *
 * 它同时负责：
 * - 打开和管理 SQLite 数据库；
 * - 注册各种 indexer；
 * - 组织 full / incremental / live 三条索引链；
 * - 对外提供搜索、统计、详情等查询接口。
 */
UCLASS()
class MONOLITHINDEX_API UMonolithIndexSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	// --- UEditorSubsystem 生命周期接口 ---
	/** 显式声明构造函数，避免 UHT 在不完整类型上内联销毁 `TUniquePtr` 成员。 */
	UMonolithIndexSubsystem();
	/** 显式声明析构函数，让 `TUniquePtr` 的删除发生在看到完整类型的 cpp 里。 */
	virtual ~UMonolithIndexSubsystem() override;
	/** 子系统初始化入口，负责打开数据库、注册 indexer、接状态。 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	/** 子系统关闭入口，负责停后台任务、解绑回调、关数据库。 */
	virtual void Deinitialize() override;

	/** 触发增量 catch-up。
	 * 它只处理“上次之后发生变化”的资产，而不是把全项目重扫一遍。 */
	UFUNCTION()
	void StartIncrementalIndex();

	/** 当前是否具备做增量索引的前提。 */
	UFUNCTION()
	bool CanDoIncrementalIndex() const;

	/** 当前有没有索引任务在运行。 */
	bool IsIndexing() const;

	/** 获取当前进度，范围 0 到 1。 */
	float GetProgress() const;

	/** 获取给 UI 展示的状态文案。 */
	FString GetStatusMessage() const { return IndexingStatusMessage; }

	/** 拿数据库对象给查询侧使用。 */
	FMonolithIndexDatabase* GetDatabase() { return Database.Get(); }

	/** 跑一段 project.* 查询。
	 * 这里直接复用子系统已经打开的主数据库连接，并通过 `DatabaseAccessMutex`
	 * 把查询和 live/incremental 写入串行化。
	 *
	 * 这样做的原因是：
	 * - 当前工程环境里“同进程第二个 SQLite 连接”不稳定；
	 * - 但主连接本身在启动时就是稳定可用的；
	 * - 统一走一份连接，才能彻底消掉 project.* 那条额外的失败路径。
	 *
	 * 数据库不可用时不会调用 Func，并返回带错误信息的 FMonolithActionResult。 */
	FMonolithActionResult RunReadDatabaseAction(
		TFunctionRef<FMonolithActionResult(FMonolithIndexDatabase&)> Func);

	/** 生成一个轻量状态栏快照。
	 * 某些贵查询只会在明确要求时才补进去。 */
	FMonolithIndexStatusBarSnapshot GetStatusBarSnapshot(bool bIncludeExpensiveDetails = false) const;

	/** 把已经在 DDC 里的 AssetVisualGeometric / AssetVisualSemantic artifact 全量 materialize 到本地 SQLite。
	 *
	 * 用法：commandlet `MonolithIndexWarmup` 跑完后，DDC 里有 artifact 但 editor 端
	 * `asset_visual_geometric / asset_visual_semantic` 表是 0 行（设计上 commandlet 不写 SQLite）。
	 * 这条入口扫 AssetRegistry 里所有支持 AssetVisual 的资产，每个走 companion path 但
	 * `bAllowLocalArtifactBuild=false`：cache 命中就 materialize 到 SQLite，cache miss 就跳过（让用户重跑 warmup）。
	 *
	 * 成本：N 次 cache lookup + 命中数次 SQLite 行写入；不会重新渲染、不会推理 CLIP。
	 * 必须在游戏线程调用。 */
	void MaterializeAssetVisualFromCache();

	/** 子进程入口：只 build artifact 写 DDC，不写 SQLite，按 shard 切分。
	 *  ShardIndex / ShardCount 由父进程传入；hash(package_path) % ShardCount == ShardIndex 的 asset 才处理。
	 *  跑完后调用方负责 RequestExit。 */
	void MaterializeAssetVisualBuildShard(int32 ShardIndex, int32 ShardCount);

	/** 父进程入口：N 路并行 build，等所有 child 退出后从 DDC merge 到 SQLite。
	 *  NumWorkers <= 1 退化为单进程同步路径；推荐 4-8（受 GPU 显存制约）。 */
	void MaterializeAssetVisualParallel(int32 NumWorkers);

	/** 工具栏入口：清空指定 cohort 的 SQLite 行。下次 Materialize 会重做。 */
	bool ClearAssetVisualCohort(FName CohortId);

	/** 当前 visual_pending.txt 里待处理的 asset 数（非 0 时状态栏 / 工具栏会提示）。 */
	int32 GetVisualPendingCount() const;

	/** 工具栏入口：等价 Monolith.StartIndex full（manual-only path）。 */
	void RequestFullIndexFromUI();
	/** 工具栏入口：等价 Monolith.StartIndex incremental，前置 baseline 检查交给调用方。 */
	void StartIncrementalIndexFromUI();
	/** 工具栏入口：当前是否能跑 incremental（有 baseline）。 */
	bool CanDoIncrementalIndexFromUI() const;

	// --- 查询接口（给 MCP action 和 UI 调用） ---
	/** 全文搜索。 */
	TArray<FSearchResult> Search(const FString& Query, int32 Limit = 50);
	/** 查某个资产的引用关系。 */
	TSharedPtr<FJsonObject> FindReferences(const FString& PackagePath);
	/** 按资产类型查列表。 */
	TArray<FIndexedAsset> FindByType(const FString& AssetClass, int32 Limit = 100, int32 Offset = 0);
	/** 读整体统计信息。 */
	TSharedPtr<FJsonObject> GetStats();
	/** 读单资产详情。 */
	TSharedPtr<FJsonObject> GetAssetDetails(const FString& PackagePath);
	/** 按尺寸范围搜索 mesh catalog。 */
	TSharedPtr<FJsonObject> SearchMeshCatalogBySize(
		const TArray<float>& MinBounds,
		const TArray<float>& MaxBounds,
		const FString& Category,
		const FString& ExcludeSizeClass,
		int32 Limit);
	/** 读取 mesh catalog 总体统计。 */
	TSharedPtr<FJsonObject> GetMeshCatalogStats();
	/** 读取当前活动 revision 下的 mesh catalog 明细，可选按路径子串过滤。 */
	TArray<FIndexedMeshCatalogEntry> GetMeshCatalogEntries(const FString& PathFilter = FString());
	/** 列出 GameplayTag 清单，可按前缀过滤。 */
	TArray<FIndexedGameplayTagSummary> ListGameplayTags(const FString& Prefix);
	/** 按子串搜索 GameplayTag，并返回引用它们的资产。 */
	TArray<FIndexedGameplayTagSummary> SearchGameplayTags(const FString& Query);
	/** 列出 stale 包；可选按 cohort 名过滤（例如 `AssetVisualGeometric`）。
	 * 空 cohort 表示返回全部 cohort 贡献的并集，行为与历史版本一致。 */
	TSharedPtr<FJsonObject> ListStalePackages(int32 Limit = 100, const FString& Cursor = FString(), const FString& CohortFilter = FString());

	/** 注册一个 indexer，并把所有权交给子系统。 */
	void RegisterIndexer(TSharedPtr<IMonolithIndexer> Indexer);

	// --- 事件委托 ---
	FOnIndexingProgress OnProgress;
	FOnIndexingComplete OnComplete;

private:
	friend struct FMonolithIndexConsoleCommands;

	/** MaterializeAssetVisualImpl 的内部参数：决定 build / materialize / shard 切分。
	 *  公开放在 header 是因为 anonymous namespace 类型不能跨 .cpp 引用。 */
	struct FMaterializeAssetVisualOpts
	{
		bool bMaterialize = true;       // 命中缓存 / build 完后是否写 SQLite
		int32 ShardIndex = 0;           // 当前进程负责的 shard 序号；ShardCount=1 表示不分片
		int32 ShardCount = 1;
	};

	/** 内部 helper：跑实际 materialize 循环；公共入口都包它。 */
	void MaterializeAssetVisualImpl(const FMaterializeAssetVisualOpts& Opts);

	/** full index 的后台任务小对象。 */
	class FIndexingTask
	{
	public:
		/** 构造 full index 后台任务对象。 */
		FIndexingTask(UMonolithIndexSubsystem* InOwner, const FString& InBuildDatabasePath);

		/** 后台线程的主执行函数。 */
		uint32 Run();
		/** 协作式停止标记。 */
		void Stop() { bShouldStop = true; }

		/** 外部请求停止时会变成 true。 */
		TAtomic<bool> bShouldStop{false};
		/** 当前处理到第几个资产。 */
		TAtomic<int32> CurrentIndex{0};
		/** 这次 full index 的总资产数。 */
		TAtomic<int32> TotalAssets{0};
		/** 这次要扫描的插件列表。 */
		TArray<FIndexedPluginInfo> PluginsToIndex;
		/** 在游戏线程抓取好的全量资产快照，后台线程只消费这份不可变数据。 */
		TArray<FAssetData> AssetsToIndex;
		/** 与快照配套的包级 saved hash，避免后台线程再访问 Asset Registry。 */
		TMap<FName, FIoHash> PackageHashes;
		/** build 数据库写到哪里。 */
		FString BuildDatabasePath;

	private:
		/** 回指外层子系统，方便调用统一逻辑。 */
		UMonolithIndexSubsystem* Owner;
	};

	/** full index 后台任务结束后的统一收尾。 */
	void OnIndexingFinished(bool bSuccess, const FString& CompletedDatabasePath = FString());
	/** scheduler 里的增量索引后台 job。 */
	void RunIncrementalIndexJob(
		TSet<FName> InCurrentPackages,
		TMap<FName, FIoHash> InCurrentHashes,
		TMap<FName, TArray<FAssetData>> InAssetsByPackage);
	/** scheduler 里的 live update 后台 job。 */
	void RunLiveUpdateJob(
		TArray<FPendingIndexChange> InRawChanges,
		TMap<FName, TArray<FAssetData>> InAssetsByPackage,
		TMap<FName, FIoHash> InCurrentHashes);
	/** live job 结束后统一收尾并决定是否重新挂回调。 */
	void FinalizeLiveDatabaseJob(bool bSuccess, bool bReRegisterLiveCallbacks, const TCHAR* SuccessMessage, const TCHAR* FailureMessage);
	/** 真正执行 full index 的内部入口，只允许手动命令链路调用。 */
	void StartFullIndexInternal();
	/** 收到手动 full index 请求后，必要时等待 Asset Registry 就绪。 */
	void RequestManualFullIndex();
	/** Asset Registry 文件加载完成后触发的启动回调。 */
	void OnAssetRegistryFilesLoaded();
	/** 注册默认内建 indexer。 */
	void RegisterDefaultIndexers();
	/** 正式数据库路径。 */
	FString GetDatabasePath() const;
	/** full index 构建中的临时数据库路径。 */
	FString GetBuildDatabasePath() const;
	/** 当前是否应该自动建索引。 */
	bool ShouldAutoIndex() const;
	/** 功能开关是否启用。 */
	bool IsIndexingEnabled() const;
	/** 当前是否启用了 shadow mode，并返回 cohort 名。 */
	bool TryGetShadowModeCohortName(FString& OutCohortName) const;
	/** 判断当前 indexer 是否还需要把 artifact 镜像写入 shadow 表。 */
	bool ShouldWriteArtifactShadowForIndexer(const IMonolithIndexer& Indexer, FString& OutCohortName) const;
	/** 对比正式表和 shadow 表的聚合摘要，决定是否有差异。 */
	void EvaluateShadowArtifactDiff(FMonolithIndexDatabase& DB, int64 AssetId, const IMonolithIndexer& Indexer) const;
	/** 记录某次资产索引成功，更新元数据、hash、历史信息。 */
	void RecordSuccessfulAssetIndex(FMonolithIndexDatabase& TargetDatabase, int64 AssetId, const FString& PackagePath, const IMonolithIndexer& Indexer, const FMonolithArtifactIdentityV1* Identity);
	/** 用“唯一的一套”正式写入流程处理一个主 indexer 资产。
	 * 它会把 revision、artifact shadow、companion、commit/diff、metadata 更新串成一次原子写入。 */
	bool TryIndexPrimaryAssetInRevision(
		const FAssetData& AssetData,
		const TSharedPtr<IMonolithIndexer>& Indexer,
		int64 AssetId,
		EMonolithArtifactCacheRequestMode RequestMode,
		FMonolithIndexDatabase& DB,
		bool& bOutDeferredToOfflineWarmup);
	/** 对没有主 deep indexer 的资产，只补跑 companion 数据。
	 * 这让 dependency / gameplay tags / 其它伴生 cohort 也能单独走 revision promote，而不是裸写生产表。 */
	bool TryIndexCompanionOnlyAssetInRevision(
		const FAssetData& AssetData,
		int64 AssetId,
		EMonolithArtifactCacheRequestMode RequestMode,
		FMonolithIndexDatabase& DB);
	/** 按稳定 id 查 indexer。
	 * 这比到处硬编码 sentinel 类名更适合做“唯一来源”。 */
	TSharedPtr<IMonolithIndexer> FindIndexerById(FName IndexerId) const;
	/** 补跑 companion indexer，例如 dependency / gameplay tag references / mesh catalog。
	 * 如果某个 companion 同时写了 shadow 数据，就把对应 indexer 收集到输出数组里，
	 * 让调用方在 commit 之后再统一做 diff。 */
	bool RunPerAssetCompanionIndexers(
		const FAssetData& AssetData,
		int64 AssetId,
		EMonolithArtifactCacheRequestMode RequestMode,
		FMonolithIndexDatabase& DB,
		TArray<TSharedPtr<IMonolithIndexer>>* OutShadowDiffIndexers = nullptr);
	/** 把一次离线 warmup 请求排进队列。 */
	bool QueueOfflineWarmupRequest(const FAssetData& AssetData, const IMonolithIndexer& Indexer, const TCHAR* Reason) const;
	/** 按 metadata 判断资产是否 stale。
	 *  单次调用便利重载：内部会从磁盘 load offline warmup queue 一次 + 单 asset metadata SQLite 查询。
	 *  在循环里大量调用时，请改用下面的批量重载，把 queue + metadata 预先批量加载，
	 *  避免 N 次 SQLite prepared statement / N 次 JSON 文件 IO。 */
	bool IsIndexedAssetStaleByMetadata(const FIndexedAsset& Asset) const;
	/** 批量调用版本：调用方先调 GetAllAssetIndexMetadata + AppendMonolithOfflineWarmupQueuedPackages
	 *  各一次，把结果传进来。函数走 TMap/TSet O(1) 命中查询，循环 N 次总成本 = O(N) 哈希查询。
	 *  PreloadedMetadata 用 nullptr 表示"调用方还没准备 metadata，回退到 SQLite 单查询"。 */
	bool IsIndexedAssetStaleByMetadata(
		const FIndexedAsset& Asset,
		const TSet<FString>& PreloadedQueuedSet,
		const TMap<int64, FMonolithAssetIndexMetadata>* PreloadedMetadata) const;
	/** 记录一次 GT 工作样本，用来做 breaker/预算统计。 */
	void RecordGtWorkSample(const IMonolithIndexer& Indexer, double DurationSeconds);
	/** 按 metadata 判断包是否 stale。 */
	bool IsPackageStaleByMetadata(const FString& PackagePath, const FString* AssetClassOverride = nullptr) const;
	/** 收集当前已知 stale 包集合。 */
	TSet<FString> GatherKnownStalePackages() const;
	/** 把运行时统计数字补进 JSON。 */
	void AppendRuntimeStats(const TSharedPtr<FJsonObject>& Stats) const;

	/** 找出需要索引的 marketplace 插件根路径。 */
	TArray<FIndexedPluginInfo> GatherMarketplacePluginPaths() const;
	/** 把插件信息转成包路径前缀。 */
	TArray<FString> BuildIndexedPackagePrefixes(const TArray<FIndexedPluginInfo>& Plugins) const;
	/** 判断某个包路径是否在我们管理的索引范围内。 */
	bool IsPackagePathIndexed(const FString& PackagePath) const;

	/** 深度索引一批资产路径。 */
	void ProcessDeepIndexQueue(
		const TMap<FName, TArray<FAssetData>>& AssetsByPackage,
		const TSet<FString>& PathsToIndex,
		EMonolithArtifactCacheRequestMode RequestMode);

	/** 对没有主 deep indexer 的包统一补跑 companion 数据。
	 * 这条 helper 现在是 full / incremental / live 共用的唯一入口：
	 * - full index 会把“没有主 indexer 的资产”塞进来；
	 * - incremental / live 会把“变化了、但没有主 indexer 的包”塞进来；
	 * - helper 内部自己做包路径排序、AR 查询、批处理事务和 revision promote。
	 *
	 * 这样 Dependency / GameplayTags / MeshCatalog / GAS 这类伴生数据
	 * 就不用再额外保留一套 scoped sentinel 旁路了。 */
	void ProcessCompanionOnlyPackages(
		const TMap<FName, TArray<FAssetData>>& AssetsByPackage,
		const TSet<FString>& PackagePaths,
		EMonolithArtifactCacheRequestMode RequestMode,
		FMonolithIndexDatabase& DB,
		const TAtomic<bool>* StopFlag = nullptr);

	/** 注册 Asset Registry 实时回调。 */
	void RegisterLiveCallbacks();

	/** 注销 Asset Registry 实时回调。 */
	void UnregisterLiveCallbacks();

	// --- Asset Registry 实时回调处理 ---
	/** 资产新增回调。 */
	void OnAssetsAddedCallback(TConstArrayView<FAssetData> Assets);
	/** 资产删除回调。 */
	void OnAssetsRemovedCallback(TConstArrayView<FAssetData> Assets);
	/** 资产改名回调。 */
	void OnAssetRenamedCallback(const FAssetData& AssetData, const FString& OldObjectPath);
	/** 磁盘上资产更新回调。 */
	void OnAssetsUpdatedOnDiskCallback(TConstArrayView<FAssetData> Assets);
	/** 把累计的 live 变化打包交给后台处理。 */
	void ProcessPendingChanges();
	/** 在 GT 上刷新右下角异步任务提示，并顺手同步取消状态。 */
	void QueueTaskNotificationProgressUpdate(int32 Current, int32 Total, const FString& ProgressText);

	// --- 实时增量跟踪状态 ---
	/** 等待处理的实时变化队列。 */
	TArray<FPendingIndexChange> PendingChanges;
	/** 合并多次变化后再触发后台处理的定时器。 */
	FTimerHandle LiveIndexTimerHandle;

	// --- Asset Registry 回调句柄 ---
	/** 新增回调句柄。 */
	FDelegateHandle OnAssetsAddedHandle;
	/** 删除回调句柄。 */
	FDelegateHandle OnAssetsRemovedHandle;
	/** 改名回调句柄。 */
	FDelegateHandle OnAssetRenamedHandle;
	/** 磁盘更新回调句柄。 */
	FDelegateHandle OnAssetsUpdatedOnDiskHandle;

	/** 当前 full index 期间缓存的插件列表。 */
	TArray<FIndexedPluginInfo> IndexedPlugins;

	/** 正式 SQLite 数据库对象。
	 * 现在查询和写入都统一走这一份连接，避免第二连接在当前工程环境下反复打开失败。 */
	TUniquePtr<FMonolithIndexDatabase> Database;
	/** artifact cache 抽象，可能落到本地或远端 DDC。 */
	TUniquePtr<IMonolithArtifactCache> ArtifactCache;
	/** 运行时计数器、breaker、历史状态。 */
	TUniquePtr<FMonolithIndexRuntimeState, FMonolithIndexRuntimeStateDeleter> RuntimeState;
	/** GT 预算状态机。 */
	FMonolithIndexGtBudgetState GtBudgetState;
	/** 所有已注册 indexer。 */
	TArray<TSharedPtr<IMonolithIndexer>> Indexers;
	/** 资产类名到 indexer 的快速映射。 */
	TMap<FString, TSharedPtr<IMonolithIndexer>> ClassToIndexer;
	/** companion indexer 按稳定 id 的映射。
	 * 它们不应该抢占真实资产类 -> 主 indexer 的分发权。 */
	TMap<FName, TSharedPtr<IMonolithIndexer>> CompanionIndexersById;

	/** 后台调度器。 */
	TUniquePtr<FMonolithIndexScheduler, FMonolithIndexSchedulerDeleter> Scheduler;
	/** full index 的任务对象。 */
	TUniquePtr<FIndexingTask> IndexingTaskPtr;
	/** 有没有后台索引在跑。 */
	TAtomic<bool> bIsIndexing{false};
	/** 手动 full index 是否还在等 Asset Registry 把文件扫完。 */
	bool bPendingManualFullIndex = false;
	/** 手动 full index 前如果已经挂着 live callbacks，失败/取消后要恢复它们。 */
	bool bRestoreLiveCallbacksAfterFullIndex = false;
	/** 保护数据库访问的互斥锁。 */
	mutable FCriticalSection DatabaseAccessMutex;

	/* GT 状态栏 tooltip 用的"昂贵字段"异步缓存。
	 *
	 * 历史背景：状态栏点击 / 0.5s 刷新 timer 同步调用 GatherKnownStalePackages()，
	 * 33840 资产规模下要扫整张 assets + asset_index_metadata 再做 set union，
	 * 每次卡 GT 几秒。修复方案：
	 *  - GetStatusBarSnapshot(true) 永不阻塞，直接读 cache
	 *  - 后台线程按 TTL 异步刷新，避免重复并发刷新
	 *  - cache 未命中时返回 INDEX_NONE，UI 已有 "—" fallback */
	mutable FCriticalSection StatusBarExpensiveCacheMutex;
	mutable int32 CachedStalePackageCount = INDEX_NONE;
	mutable int32 CachedOfflineQueueDepth = INDEX_NONE;
	mutable double CachedStatusBarExpensiveAtSeconds = -1.0;
	mutable TAtomic<bool> bStatusBarExpensiveRefreshInFlight{false};

	/** 后台异步刷新 status bar 的"昂贵字段"，TTL 命中直接 noop。 */
	void KickStatusBarExpensiveRefresh() const;

	/** 给 UI 和日志展示的当前状态文字。 */
	FString IndexingStatusMessage;
	/** 编辑器角落里弹出的异步任务提示。 */
	TUniquePtr<FAsyncTaskNotification> TaskNotification;
	/** 右下角任务提示是否已经被用户点了 Cancel。 */
	TAtomic<bool> bTaskNotificationCancelRequested{false};
};
