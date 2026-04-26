#include "Commandlets/MonolithIndexWarmupCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Commandlets/MonolithIndexCommandletSupport.h"
#include "Indexers/AnimationIndexer.h"
#include "Indexers/BehaviorTreeIndexer.h"
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/ConfigIndexer.h"
#include "Indexers/CppIndexer.h"
#include "Indexers/DataAssetIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/GenericAssetIndexer.h"
#include "Indexers/GameplayTagDefinitionIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/InputActionIndexer.h"
#include "Indexers/LevelIndexer.h"
#include "Indexers/MaterialIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Indexers/AssetVisualGeometricIndexer.h"
#include "Indexers/AssetVisualSemanticIndexer.h"
#include "Embedders/ClipSemanticEmbeddingProvider.h"
#include "Embedders/GeometricEmbeddingProvider.h"
#include "AssetVisualEmbeddingProvider.h"
#include "Indexers/NiagaraIndexer.h"
#include "Indexers/EQSIndexer.h"
#if WITH_STATETREE
#include "Indexers/StateTreeIndexer.h"
#endif
#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/UserDefinedStructIndexer.h"
#include "MonolithArtifactCache.h"
#include "MonolithArtifactTypes.h"
#include "MonolithAssetArtifactPipeline.h"
#include "MonolithGlobalArtifactPipeline.h"
#include "MonolithIndexer.h"
#include "MonolithIndexLog.h"
#include "MonolithOfflineWarmupQueue.h"
#include "MonolithSettings.h"
#include "Commandlets/MonolithWarmupHistory.h"

/*
 * 这个 commandlet 的任务可以概括成三步：
 * 1. 找出这次 scope 下应该尝试 warmup 的资产或全局 reducer；
 * 2. 为它们构建 identity，并优先从 artifact cache 取；
 * 3. 把这次命中率结果写进 warmup history，供 release gate 连续观察。
 *
 * 它不会写本地 SQLite，
 * 所以更像“把缓存预先填热”，而不是“重建索引数据库”。
 */

namespace MonolithWarmupCommandletInternal
{
	/** commandlet 侧登记的一条 artifact warmup 索引器记录。 */
	struct FRegisteredWarmupIndexer
	{
		/** 真实 indexer 实例。 */
		TSharedPtr<IMonolithIndexer> Indexer;
		/** 是否允许通过“资产类 -> 主 indexer”这条分发表命中。 */
		bool bAllowClassDispatch = false;
		/** 是否允许在显式 cohort scope 下，对所有资产逐个尝试。 */
		bool bAllowExplicitAssetWideScope = false;
	};

	/** 把 scope 结构体转成稳定可读字符串，便于日志和历史记录复用。 */
	static FString DescribeScope(const FMonolithWarmupScope& Scope)
	{
		switch (Scope.Kind)
		{
		case EMonolithWarmupScopeKind::OfflineOnly:
			return TEXT("OfflineOnly");
		case EMonolithWarmupScopeKind::All:
			return TEXT("All");
		case EMonolithWarmupScopeKind::GlobalReducer:
			return TEXT("GlobalReducer");
		case EMonolithWarmupScopeKind::Cohort:
		default:
			return FString::Printf(TEXT("Cohort:%s"), *Scope.CohortName.ToString());
		}
	}

	/*
	 * 注册所有“已经支持 artifact”的 indexer。
	 *
	 * warmup 只对 artifact-capable 的 indexer 有意义，
	 * 因为它的目标就是提前构建 artifact 并写进缓存。
	 */
	static void RegisterArtifactCapableIndexers(
		TArray<FRegisteredWarmupIndexer>& OutIndexers,
		TMap<FString, TSharedPtr<IMonolithIndexer>>& OutClassToIndexer,
		TMap<FString, TSharedPtr<IMonolithIndexer>>& OutIndexerIdToIndexer)
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		auto RegisterIndexerInstance = [&OutIndexers, &OutClassToIndexer, &OutIndexerIdToIndexer](
			const TSharedPtr<IMonolithIndexer>& Indexer,
			const bool bAllowClassDispatch,
			const bool bAllowExplicitAssetWideScope)
		{
			if (!Indexer.IsValid())
			{
				return;
			}

			FRegisteredWarmupIndexer RegisteredIndexer;
			RegisteredIndexer.Indexer = Indexer;
			RegisteredIndexer.bAllowClassDispatch = bAllowClassDispatch;
			RegisteredIndexer.bAllowExplicitAssetWideScope = bAllowExplicitAssetWideScope;
			OutIndexers.Add(MoveTemp(RegisteredIndexer));
			// 两张表各有用途：
			// - class -> indexer：给主 cohort 的普通扫描路径使用；
			// - indexer id -> indexer：给离线队列和显式 cohort 查找使用。
			OutIndexerIdToIndexer.Add(Indexer->GetIndexerId().ToString(), Indexer);
			if (bAllowClassDispatch)
			{
				for (const FString& ClassName : Indexer->GetSupportedClasses())
				{
					OutClassToIndexer.Add(ClassName, Indexer);
				}
			}
		};

		if (Settings && Settings->bIndexGenericAssets)
		{
			RegisterIndexerInstance(MakeShared<FGenericAssetIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexBlueprints)
		{
			RegisterIndexerInstance(MakeShared<FBlueprintIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexLevels)
		{
			RegisterIndexerInstance(MakeShared<FLevelIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexMaterials)
		{
			RegisterIndexerInstance(MakeShared<FMaterialIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexDataTables)
		{
			RegisterIndexerInstance(MakeShared<FDataTableIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexAnimations)
		{
			RegisterIndexerInstance(MakeShared<FAnimationIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexBehaviorTrees)
		{
			RegisterIndexerInstance(MakeShared<FBehaviorTreeIndexer>(), true, false);
			RegisterIndexerInstance(MakeShared<FEQSIndexer>(), true, false);
		}

#if WITH_STATETREE
		if (Settings && Settings->bIndexStateTrees)
		{
			RegisterIndexerInstance(MakeShared<FStateTreeIndexer>(), true, false);
		}
#endif

		if (Settings && Settings->bIndexNiagara)
		{
			RegisterIndexerInstance(MakeShared<FNiagaraIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexInputActions)
		{
			RegisterIndexerInstance(MakeShared<FInputActionIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexUserDefinedEnums)
		{
			RegisterIndexerInstance(MakeShared<FUserDefinedEnumIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexUserDefinedStructs)
		{
			RegisterIndexerInstance(MakeShared<FUserDefinedStructIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexDataAssets)
		{
			RegisterIndexerInstance(MakeShared<FDataAssetIndexer>(), true, false);
		}

		if (Settings && Settings->bIndexMeshCatalog)
		{
			// MeshCatalog 是 StaticMesh 的 companion。
			// 它不应该覆盖 GenericAsset 的主 class dispatch，
			// 但显式 `Scope=Cohort:MeshCatalog` 时应该允许逐资产 warmup。
			RegisterIndexerInstance(MakeShared<FMeshCatalogIndexer>(), false, true);
		}

		// AssetVisual 双 cohort 与 MeshCatalog 同样属于 StaticMesh companion，
		// 必须由 -Scope=Cohort:AssetVisualGeometric / AssetVisualSemantic 显式触发。
		// commandlet 进程独立于编辑器，必须自己再注册一次 provider 单例。
		if (Settings && Settings->bIndexAssetVisualGeometric)
		{
			if (!FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(FName(TEXT("geometric_v1"))).IsValid())
			{
				FAssetVisualEmbeddingProviderRegistry::Get().RegisterProvider(MakeShared<FGeometricEmbeddingProvider>());
			}
			RegisterIndexerInstance(MakeShared<FAssetVisualGeometricIndexer>(), false, true);
		}
		if (Settings && Settings->bIndexAssetVisualSemantic)
		{
			if (!FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(FName(TEXT("clip_vit_b32_v1"))).IsValid())
			{
				FAssetVisualEmbeddingProviderRegistry::Get().RegisterProvider(MakeShared<FClipSemanticEmbeddingProvider>());
			}
			RegisterIndexerInstance(MakeShared<FAssetVisualSemanticIndexer>(), false, true);
		}

		if (Settings && Settings->bIndexGAS)
		{
			// GAS 和 MeshCatalog 一样，也是 companion：
			// - 不参与 Blueprint 的主 class dispatch；
			// - 但在显式 `Scope=Cohort:GAS` 时，应该允许逐资产尝试 warmup。
			RegisterIndexerInstance(MakeShared<FGASIndexer>(), false, true);
		}

		if (Settings && Settings->bIndexDependencies)
		{
			// companion cohort 没有“资产类 -> 主 indexer”分发入口，
			// 但显式 `Scope=Cohort:Dependency` 时，仍然应该对每个资产尝试 warmup。
			RegisterIndexerInstance(MakeShared<FDependencyIndexer>(), false, true);
		}

		if (Settings && Settings->bIndexGameplayTags)
		{
			RegisterIndexerInstance(MakeShared<FGameplayTagIndexer>(), false, true);
			RegisterIndexerInstance(MakeShared<FGameplayTagDefinitionIndexer>(), false, false);
		}

		if (Settings && Settings->bIndexConfigs)
		{
			RegisterIndexerInstance(MakeShared<FConfigIndexer>(), false, false);
		}

		if (Settings && Settings->bIndexCppSymbols)
		{
			RegisterIndexerInstance(MakeShared<FCppIndexer>(), false, false);
		}
	}

	/** 为一份资产收集这次 warmup 应该尝试的 indexer。 */
	static void CollectWarmupIndexersForAsset(
		const FAssetData& AssetData,
		const FMonolithWarmupScope& Scope,
		const TArray<FRegisteredWarmupIndexer>& RegisteredIndexers,
		const TMap<FString, TSharedPtr<IMonolithIndexer>>& ClassToIndexer,
		TArray<TSharedPtr<IMonolithIndexer>>& OutIndexers)
	{
		OutIndexers.Reset();

		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		if (const TSharedPtr<IMonolithIndexer>* PrimaryIndexer = ClassToIndexer.Find(ClassName))
		{
			if (PrimaryIndexer->IsValid()
				&& (*PrimaryIndexer)->MatchesAsset(AssetData)
				&& DoesMonolithWarmupScopeTargetIndexer(
					Scope,
					(*PrimaryIndexer)->GetIndexerId(),
					(*PrimaryIndexer)->GetName(),
					(*PrimaryIndexer)->GetExecutionMode()))
			{
				OutIndexers.Add(*PrimaryIndexer);
			}
		}

		// companion cohort 没有 class dispatch，只有在显式点名时才对每个资产逐个尝试。
		if (Scope.Kind != EMonolithWarmupScopeKind::Cohort)
		{
			return;
		}

		for (const FRegisteredWarmupIndexer& RegisteredIndexer : RegisteredIndexers)
		{
			if (!RegisteredIndexer.bAllowExplicitAssetWideScope || !RegisteredIndexer.Indexer.IsValid())
			{
				continue;
			}
			if (!DoesMonolithWarmupScopeTargetIndexer(
				Scope,
				RegisteredIndexer.Indexer->GetIndexerId(),
				RegisteredIndexer.Indexer->GetName(),
				RegisteredIndexer.Indexer->GetExecutionMode()))
			{
				continue;
			}

			// companion 的“到底命不中这份资产”现在统一交给 indexer 自己判断。
			// 这样像 GAS 这种要看 Blueprint 父类标签的 companion，
			// 就不需要再在 warmup 里复制一份特殊规则。
			if (!RegisteredIndexer.Indexer->MatchesAsset(AssetData))
			{
				continue;
			}

			OutIndexers.AddUnique(RegisteredIndexer.Indexer);
		}
	}

	/*
	 * 尝试给单个资产做 warmup。
	 *
	 * 这里的“成功”包括两种情况：
	 * - cache 里本来就有；
	 * - cache 里没有，但这次成功构建并写入了。
	 */
	static bool TryWarmAsset(
		const FAssetData& AssetData,
		const TSharedPtr<IMonolithIndexer>& Indexer,
		IAssetRegistry& AssetRegistry,
		IMonolithArtifactCache& ArtifactCache)
	{
		if (!Indexer.IsValid())
		{
			return false;
		}

		(void)AssetRegistry;

		MonolithAssetArtifactPipeline::FExecuteAssetOptions ExecuteOptions;
		ExecuteOptions.RequestMode = EMonolithArtifactCacheRequestMode::Warmup;
		ExecuteOptions.bAllowLocalArtifactBuild = true;
		ExecuteOptions.bMaterializeProduction = false;

		MonolithAssetArtifactPipeline::FExecuteAssetResult ExecuteResult;
		return MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
			AssetData,
			nullptr,
			[AssetData]() -> UObject*
			{
				// warmup 只有在 cache miss 且 indexer 确实需要 LoadedAsset 时，才真正触发加载。
				return AssetData.GetAsset();
			},
			*Indexer,
			&ArtifactCache,
			nullptr,
			ExecuteOptions,
			ExecuteResult) == MonolithAssetArtifactPipeline::EExecuteAssetOutcome::Succeeded;
	}

	/** 从离线 warmup 队列请求里找到对应的资产。 */
	static bool FindQueuedAsset(
		IAssetRegistry& AssetRegistry,
		const FMonolithOfflineWarmupRequest& Request,
		FAssetData& OutAssetData)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*Request.PackagePath), Assets);
		if (Assets.Num() == 0)
		{
			return false;
		}

		for (const FAssetData& AssetData : Assets)
		{
			if (Request.AssetClass.IsEmpty() || AssetData.AssetClassPath.GetAssetName().ToString() == Request.AssetClass)
			{
				OutAssetData = AssetData;
				return true;
			}
		}

		OutAssetData = Assets[0];
		return true;
	}

	/** 给单个全局 reducer 做 warmup。
	 *
	 * 这条路径不会打开本地 SQLite，
	 * 只负责“如果 cache 里没有，就把 artifact 先放进去”。 */
	static bool TryWarmGlobalIndexer(
		const TSharedPtr<IMonolithIndexer>& Indexer,
		IMonolithArtifactCache& ArtifactCache)
	{
		if (!Indexer.IsValid())
		{
			return false;
		}

		return MonolithGlobalArtifactPipeline::WarmGlobalIndexerArtifact(*Indexer, ArtifactCache);
	}
}

UMonolithIndexWarmupCommandlet::UMonolithIndexWarmupCommandlet()
{
	// 这是纯命令行工具，不需要 server/client 语义，但要有 editor 环境。
	LogToConsole = true;
	IsServer = false;
	IsClient = false;
	IsEditor = true;
	HelpDescription = TEXT("MonolithIndex warmup commandlet. 只写 DDC，不写本地 SQLite。");
}

int32 UMonolithIndexWarmupCommandlet::Main(const FString& Params)
{
	// 先把命令行解析成结构体，后面逻辑都只读 Args。
	FMonolithWarmupCommandletArgs Args;
	FString ParseError;
	if (!ParseMonolithWarmupCommandletArgs(Params, Args, ParseError))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("%s"), *ParseError);
		return 1;
	}

	IAssetRegistry* AssetRegistry = nullptr;
	if (Args.Scope.Kind != EMonolithWarmupScopeKind::GlobalReducer)
	{
		AssetRegistry = &FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		if (!AssetRegistry->IsSearchAllAssets())
		{
			AssetRegistry->SearchAllAssets(true);
		}
		AssetRegistry->WaitForCompletion();
	}

	TArray<MonolithWarmupCommandletInternal::FRegisteredWarmupIndexer> Indexers;
	TMap<FString, TSharedPtr<IMonolithIndexer>> ClassToIndexer;
	TMap<FString, TSharedPtr<IMonolithIndexer>> IndexerIdToIndexer;
	MonolithWarmupCommandletInternal::RegisterArtifactCapableIndexers(Indexers, ClassToIndexer, IndexerIdToIndexer);

	TUniquePtr<IMonolithArtifactCache> ArtifactCache = MakeUnique<FMonolithDdcArtifactCache>(FString(MonolithCacheBuckets::Default));
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const double StartSeconds = FPlatformTime::Seconds();
	int32 WarmedCount = 0;
	int32 AttemptedCount = 0;
	int32 RemovedFromOfflineQueue = 0;
	int32 RemainingOfflineQueue = 0;

	if (Args.Scope.Kind == EMonolithWarmupScopeKind::OfflineOnly)
	{
		// OfflineOnly 不扫全项目，而是只消费之前排进离线队列的请求。
		TArray<FMonolithOfflineWarmupRequest> PendingRequests;
		if (!LoadMonolithOfflineWarmupQueue(PendingRequests))
		{
			UE_LOG(LogMonolithIndex, Error, TEXT("读取离线 warmup 队列失败"));
			return 1;
		}

		TArray<FMonolithOfflineWarmupRequest> CompletedRequests;
		for (const FMonolithOfflineWarmupRequest& Request : PendingRequests)
		{
			if (Args.MaxPackages > 0 && AttemptedCount >= Args.MaxPackages)
			{
				break;
			}
			if (Args.ShouldStopForTimeWindow(StartSeconds, FPlatformTime::Seconds()))
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("达到 TimeWindowMinutes 限制，提前退出 warmup"));
				break;
			}

			const TSharedPtr<IMonolithIndexer>* FoundIndexer = IndexerIdToIndexer.Find(Request.IndexerId);
			if (!FoundIndexer || !FoundIndexer->IsValid())
			{
				// 队列里引用了当前未注册的 indexer，就跳过。
				continue;
			}

			FAssetData AssetData;
			if (!AssetRegistry || !MonolithWarmupCommandletInternal::FindQueuedAsset(*AssetRegistry, Request, AssetData))
			{
				continue;
			}

			++AttemptedCount;
			if (MonolithWarmupCommandletInternal::TryWarmAsset(
				AssetData,
				*FoundIndexer,
				*AssetRegistry,
				*ArtifactCache))
			{
				++WarmedCount;
				CompletedRequests.Add(Request);
			}
		}

		RemovedFromOfflineQueue = RemoveMonolithOfflineWarmupRequests(CompletedRequests);
		TArray<FMonolithOfflineWarmupRequest> RemainingRequests;
		LoadMonolithOfflineWarmupQueue(RemainingRequests);
		RemainingOfflineQueue = RemainingRequests.Num();
	}
	else
	{
		if (Args.Scope.Kind == EMonolithWarmupScopeKind::GlobalReducer)
		{
			for (const MonolithWarmupCommandletInternal::FRegisteredWarmupIndexer& RegisteredIndexer : Indexers)
			{
				if (!RegisteredIndexer.Indexer.IsValid())
				{
					continue;
				}
				if (!DoesMonolithWarmupScopeTargetIndexer(
					Args.Scope,
					RegisteredIndexer.Indexer->GetIndexerId(),
					RegisteredIndexer.Indexer->GetName(),
					RegisteredIndexer.Indexer->GetExecutionMode()))
				{
					continue;
				}
				if (Args.MaxPackages > 0 && AttemptedCount >= Args.MaxPackages)
				{
					break;
				}
				if (Args.ShouldStopForTimeWindow(StartSeconds, FPlatformTime::Seconds()))
				{
					UE_LOG(LogMonolithIndex, Warning, TEXT("达到 TimeWindowMinutes 限制，提前退出 warmup"));
					break;
				}

				++AttemptedCount;
				if (MonolithWarmupCommandletInternal::TryWarmGlobalIndexer(RegisteredIndexer.Indexer, *ArtifactCache))
				{
					++WarmedCount;
				}
			}
		}
		else
		{
		// 只有非 OfflineOnly 范围才需要先枚举全项目资产。
		// 这样可以避免离线队列模式下做一次完全没必要的全量 AR 遍历。
		TArray<FAssetData> Assets;
		FARFilter Filter;
		for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
		{
			Filter.PackagePaths.Add(ContentPath);
		}
		Filter.bRecursivePaths = true;
		AssetRegistry->GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});

		// 其它 scope 直接按收集到的全量资产扫描，再根据 class/indexer/scope 做筛选。
		for (const FAssetData& AssetData : Assets)
		{
			if (Args.MaxPackages > 0 && AttemptedCount >= Args.MaxPackages)
			{
				break;
			}
			if (Args.ShouldStopForTimeWindow(StartSeconds, FPlatformTime::Seconds()))
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("达到 TimeWindowMinutes 限制，提前退出 warmup"));
				break;
			}

			TArray<TSharedPtr<IMonolithIndexer>> AssetIndexers;
			MonolithWarmupCommandletInternal::CollectWarmupIndexersForAsset(
				AssetData,
				Args.Scope,
				Indexers,
				ClassToIndexer,
				AssetIndexers);
			if (AssetIndexers.Num() == 0)
			{
				continue;
			}

			bool bShouldBreak = false;
			for (const TSharedPtr<IMonolithIndexer>& Indexer : AssetIndexers)
			{
				if (!Indexer.IsValid())
				{
					continue;
				}
				if (Args.MaxPackages > 0 && AttemptedCount >= Args.MaxPackages)
				{
					bShouldBreak = true;
					break;
				}
				if (Args.ShouldStopForTimeWindow(StartSeconds, FPlatformTime::Seconds()))
				{
					UE_LOG(LogMonolithIndex, Warning, TEXT("达到 TimeWindowMinutes 限制，提前退出 warmup"));
					bShouldBreak = true;
					break;
				}

				++AttemptedCount;
				if (MonolithWarmupCommandletInternal::TryWarmAsset(
					AssetData,
					Indexer,
					*AssetRegistry,
					*ArtifactCache))
				{
					++WarmedCount;
				}
			}

			if (bShouldBreak)
			{
				break;
			}
		}
		}
	}

	// commandlet 的目标就是把缓存预热完整，
	// 所以在读取统计和写 history 之前，要先把待发的远端写尽量收尾。
	if (!ArtifactCache->DrainRemoteWrites(30.0))
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("warmup remote write queue did not fully drain before stats snapshot"));
	}

	const FMonolithArtifactCacheStats Stats = ArtifactCache->GetStats();
	// hit rate 和 history 都是 release gate 后续判断的输入。
	const double HitRatePercent = ComputeMonolithWarmupHitRatePercent(Stats, AttemptedCount);
	const int32 ReleaseThreshold = NormalizeMonolithWarmupReleaseThresholdPercent(
		Settings ? Settings->WarmupReleaseThreshold : 90);
	const FString ScopeDisplay = MonolithWarmupCommandletInternal::DescribeScope(Args.Scope);
	const FString HistoryPath = GetMonolithWarmupHistoryPath();
	FMonolithWarmupRunRecord RunRecord;
	RunRecord.ScopeKey = ScopeDisplay;
	RunRecord.ScopeDisplay = ScopeDisplay;
	RunRecord.StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	RunRecord.AttemptedPackages = AttemptedCount;
	RunRecord.WarmedPackages = WarmedCount;
	RunRecord.CacheHitRatePercent = HitRatePercent;
	RunRecord.LocalHitCount = Stats.LocalHitCount;
	RunRecord.RemoteHitCount = Stats.RemoteHitCount;
	RunRecord.RemoteMissCount = Stats.RemoteMissCount;
	RunRecord.RemoteWriteOkCount = Stats.RemoteWriteOkCount;
	RunRecord.RemoteWriteFailCount = Stats.RemoteWriteFailCount;
	RunRecord.bThresholdMet = AttemptedCount > 0 && HitRatePercent >= static_cast<double>(ReleaseThreshold);

	if (!AppendMonolithWarmupHistoryRecord(HistoryPath, RunRecord))
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("写入 warmup 历史失败: %s"), *HistoryPath);
	}

	TArray<FMonolithWarmupRunRecord> History;
	int32 ConsecutivePasses = 0;
	if (LoadMonolithWarmupHistory(HistoryPath, History))
	{
		ConsecutivePasses = CountConsecutiveThresholdPassingWarmupRuns(
			History,
			RunRecord.ScopeKey,
			static_cast<double>(ReleaseThreshold));
	}
	else
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("读取 warmup 历史失败: %s"), *HistoryPath);
	}

	// 当前 gate 策略：同一个 scope 连续两次达标才算 ready。
	const bool bReleaseGateReady = ConsecutivePasses >= 2;
	UE_LOG(
		LogMonolithIndex,
		Log,
		TEXT("Warmup 完成: attempted=%d warmed=%d hit_rate=%.2f%% local_hit=%llu remote_hit=%llu remote_miss=%llu remote_write_ok=%llu remote_write_fail=%llu offline_queue_removed=%d offline_queue_remaining=%d"),
		AttemptedCount,
		WarmedCount,
		HitRatePercent,
		Stats.LocalHitCount,
		Stats.RemoteHitCount,
		Stats.RemoteMissCount,
		Stats.RemoteWriteOkCount,
		Stats.RemoteWriteFailCount,
		RemovedFromOfflineQueue,
		RemainingOfflineQueue);
	UE_LOG(
		LogMonolithIndex,
		Log,
		TEXT("Warmup release gate: scope=%s threshold=%d consecutive_passes=%d ready=%s history=%s"),
		*ScopeDisplay,
		ReleaseThreshold,
		ConsecutivePasses,
		bReleaseGateReady ? TEXT("true") : TEXT("false"),
		*HistoryPath);

	return 0;
}
