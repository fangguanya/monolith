#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexerShadowMode.h"
#include "MonolithIndexScheduler.h"
#include "MonolithMemoryHelper.h"
#include "MonolithAssetArtifactPipeline.h"
#include "Commandlets/MonolithIndexCommandletSupport.h"
#include "MonolithGlobalArtifactPipeline.h"
#include "MonolithIndexGtBudget.h"
#include "MonolithOfflineWarmupQueue.h"
#include "MonolithIndexRuntimeState.h"
#include "Misc/AsyncTaskNotification.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "IO/IoHash.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Application/SlateApplication.h"

// Indexers
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/MaterialIndexer.h"
#include "Indexers/GenericAssetIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/LevelIndexer.h"
#include "Indexers/ConfigIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/GameplayTagDefinitionIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/CppIndexer.h"
#include "Indexers/AnimationIndexer.h"
#include "Indexers/NiagaraIndexer.h"
#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/UserDefinedStructIndexer.h"
#include "Indexers/InputActionIndexer.h"
#include "Indexers/DataAssetIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Indexers/AssetVisualGeometricIndexer.h"
#include "Indexers/AssetVisualSemanticIndexer.h"
#include "Embedders/ClipSemanticEmbeddingProvider.h"
#include "Embedders/GeometricEmbeddingProvider.h"
#include "AssetVisualEmbeddingProvider.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/BehaviorTreeIndexer.h"
#include "Indexers/EQSIndexer.h"
#if WITH_STATETREE
#include "Indexers/StateTreeIndexer.h"
#endif

/*
 * 这是 MonolithIndex 最核心、也最“像交通枢纽”的实现文件。
 *
 * 它把下面这些东西接在一起：
 * - Asset Registry：告诉我们项目里现在有哪些资产；
 * - 各种 Indexer：负责把资产翻译成统一的索引结果；
 * - Scheduler：把后台线程和 IO/DDC 线程池组织起来；
 * - Database：保存最近一次“已经提交”的本地快照；
 * - RuntimeState：给查询和状态栏提供“现在正在忙什么”的运行中信息。
 *
 * 这份文件里最重要的设计目标只有一句话：
 * “索引可以慢慢更新，但查询不能因为更新过程而突然失明。”
 *
 * 所以你会看到：
 * - full / incremental / live 三条写路径最后都会收口到数据库快照；
 * - deep index 现在尽量停留在后台链路，真正需要 LoadedAsset 的构建统一交给 warmup；
 * - 某些全局 sentinel 已经开始拆成 per-asset companion 路径，
 *   这样 live 更新时就不必整表清空重建。
 */

namespace MonolithIndexInternal
{
	static FString MakeBuildDatabasePath(const FString& DatabasePath)
	{
		const FString Directory = FPaths::GetPath(DatabasePath);
		const FString BaseName = FPaths::GetBaseFilename(DatabasePath);
		return Directory / FString::Printf(TEXT("%s.building.db"), *BaseName);
	}

	/** 为带自定义 deleter 的运行时状态创建唯一入口，避免到处散落裸 `new`。 */
	static TUniquePtr<FMonolithIndexRuntimeState, FMonolithIndexRuntimeStateDeleter> MakeRuntimeState()
	{
		return TUniquePtr<FMonolithIndexRuntimeState, FMonolithIndexRuntimeStateDeleter>(new FMonolithIndexRuntimeState());
	}

	/** 为带自定义 deleter 的 scheduler 创建唯一入口。 */
	static TUniquePtr<FMonolithIndexScheduler, FMonolithIndexSchedulerDeleter> MakeScheduler()
	{
		return TUniquePtr<FMonolithIndexScheduler, FMonolithIndexSchedulerDeleter>(new FMonolithIndexScheduler());
	}

	static TSet<FString> CollectPackagePaths(const TArray<FAssetData>& Assets)
	{
		TSet<FString> Result;
		for (const FAssetData& AssetData : Assets)
		{
			Result.Add(AssetData.PackageName.ToString());
		}
		return Result;
	}

	static TSet<FString> CollectPackagePaths(const TSet<FName>& Packages)
	{
		TSet<FString> Result;
		for (const FName& PackageName : Packages)
		{
			Result.Add(PackageName.ToString());
		}
		return Result;
	}

	static void AddPackagePaths(TSet<FString>& Destination, const TSet<FName>& Packages)
	{
		for (const FName& PackageName : Packages)
		{
			Destination.Add(PackageName.ToString());
		}
	}

	static void AddPackagePaths(TSet<FString>& Destination, const TArray<FName>& Packages)
	{
		for (const FName& PackageName : Packages)
		{
			Destination.Add(PackageName.ToString());
		}
	}

	static void AddPackagePaths(TSet<FString>& Destination, const TArray<TPair<FName, FName>>& Moves)
	{
		for (const TPair<FName, FName>& Move : Moves)
		{
			Destination.Add(Move.Key.ToString());
			Destination.Add(Move.Value.ToString());
		}
	}

	static void AccumulateShadowAggregate(FMonolithShadowAggregate& Destination, const uint64 RowCount, const uint64 RowHashSum)
	{
		Destination.RowCount += RowCount;
		Destination.RowHashSum += RowHashSum;
	}

	/** 把包路径前缀转成统一的递归 Asset Registry filter。 */
	static FARFilter BuildIndexedAssetFilter(const TArray<FString>& IndexedPrefixes)
	{
		FARFilter Filter;
		for (const FString& Prefix : IndexedPrefixes)
		{
			Filter.PackagePaths.AddUnique(FName(*Prefix));
		}
		Filter.bRecursivePaths = true;
		return Filter;
	}

	/** 对 `FAssetData` 做稳定排序，保证 full / incremental / live 三条链的输入顺序一致。 */
	static void SortAssetSnapshot(TArray<FAssetData>& Assets)
	{
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			if (A.PackageName != B.PackageName)
			{
				return A.PackageName.LexicalLess(B.PackageName);
			}
			if (A.AssetName != B.AssetName)
			{
				return A.AssetName.LexicalLess(B.AssetName);
			}
			return A.AssetClassPath.ToString() < B.AssetClassPath.ToString();
		});
	}

	/** 用一套唯一 helper 在 GT 上抓取“资产 + 包 + hash + 按包分组”的快照。 */
	static void CollectManagedAssetSnapshotFromRegistry(
		IAssetRegistry& AssetRegistry,
		const TArray<FString>& IndexedPrefixes,
		TArray<FAssetData>* OutAssets = nullptr,
		TSet<FName>* OutPackages = nullptr,
		TMap<FName, FIoHash>* OutHashes = nullptr,
		TMap<FName, TArray<FAssetData>>* OutAssetsByPackage = nullptr)
	{
		check(IsInGameThread());

		if (OutAssets)
		{
			OutAssets->Reset();
		}
		if (OutPackages)
		{
			OutPackages->Reset();
		}
		if (OutHashes)
		{
			OutHashes->Reset();
		}
		if (OutAssetsByPackage)
		{
			OutAssetsByPackage->Reset();
		}

		AssetRegistry.EnumerateAssets(BuildIndexedAssetFilter(IndexedPrefixes), [&AssetRegistry, OutAssets, OutPackages, OutHashes, OutAssetsByPackage](const FAssetData& AssetData)
		{
			if (OutAssets)
			{
				OutAssets->Add(AssetData);
			}
			if (OutAssetsByPackage)
			{
				OutAssetsByPackage->FindOrAdd(AssetData.PackageName).Add(AssetData);
			}

			const FName PackageName = AssetData.PackageName;
			const bool bNeedPackageMetadata = OutPackages || OutHashes;
			const bool bHasPackageMetadata =
				(OutPackages && OutPackages->Contains(PackageName))
				|| (OutHashes && OutHashes->Contains(PackageName));
			const bool bIsNewPackage = !bHasPackageMetadata;
			if (bNeedPackageMetadata && bIsNewPackage)
			{
				if (OutPackages)
				{
					OutPackages->Add(PackageName);
				}
				if (OutHashes)
				{
					if (const TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(PackageName))
					{
						OutHashes->Add(PackageName, PackageData->GetPackageSavedHash());
					}
				}
			}
			return true;
		});

		if (OutAssets)
		{
			SortAssetSnapshot(*OutAssets);
		}
		if (OutAssetsByPackage)
		{
			for (TPair<FName, TArray<FAssetData>>& Pair : *OutAssetsByPackage)
			{
				SortAssetSnapshot(Pair.Value);
			}
		}
	}

	/** 把一组包名展开成真实资产快照，供 live 更新在后台线程继续处理。 */
	static void CollectPackageAssetSnapshotFromRegistry(
		IAssetRegistry& AssetRegistry,
		const TSet<FName>& PackageNames,
		TMap<FName, TArray<FAssetData>>& OutAssetsByPackage,
		TMap<FName, FIoHash>* OutHashes = nullptr)
	{
		check(IsInGameThread());
		OutAssetsByPackage.Reset();
		if (OutHashes)
		{
			OutHashes->Reset();
		}

		for (const FName PackageName : PackageNames)
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(PackageName, Assets);
			if (Assets.Num() == 0)
			{
				continue;
			}

			SortAssetSnapshot(Assets);
			OutAssetsByPackage.Add(PackageName, MoveTemp(Assets));

			if (OutHashes)
			{
				if (const TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(PackageName))
				{
					OutHashes->Add(PackageName, PackageData->GetPackageSavedHash());
				}
			}
		}
	}

	/** 从已经抓取好的资产数组回填按包分组视图，避免 full index 再回头读 Asset Registry。 */
	static void BuildAssetsByPackageSnapshot(
		const TArray<FAssetData>& Assets,
		TMap<FName, TArray<FAssetData>>& OutAssetsByPackage)
	{
		OutAssetsByPackage.Reset();
		for (const FAssetData& AssetData : Assets)
		{
			OutAssetsByPackage.FindOrAdd(AssetData.PackageName).Add(AssetData);
		}
		for (TPair<FName, TArray<FAssetData>>& Pair : OutAssetsByPackage)
		{
			SortAssetSnapshot(Pair.Value);
		}
	}

	/** 统一从包路径解析所属模块名，避免三条写路径各自写一套判断。 */
	static FString ResolveModuleNameForPackagePath(
		const FString& PackagePath,
		const TArray<FIndexedPluginInfo>& IndexedPlugins)
	{
		if (PackagePath.StartsWith(TEXT("/Game/")))
		{
			return FString();
		}

		for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
		{
			if (PackagePath.StartsWith(PluginInfo.MountPath))
			{
				return PluginInfo.PluginName;
			}
		}

		const int32 SecondSlash = PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		return SecondSlash > 1
			? PackagePath.Mid(1, SecondSlash - 1)
			: FString();
	}

	/** 把 `FAssetData` 转成数据库里的 `FIndexedAsset` 元数据记录。 */
	static FIndexedAsset BuildIndexedAssetRecord(
		const FAssetData& AssetData,
		const TArray<FIndexedPluginInfo>& IndexedPlugins,
		const TMap<FName, FIoHash>* PackageHashes = nullptr)
	{
		FIndexedAsset IndexedAsset;
		IndexedAsset.PackagePath = AssetData.PackageName.ToString();
		IndexedAsset.AssetName = AssetData.AssetName.ToString();
		IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
		IndexedAsset.ModuleName = ResolveModuleNameForPackagePath(IndexedAsset.PackagePath, IndexedPlugins);

		FString PackageFilename;
		if (FPackageName::DoesPackageExist(IndexedAsset.PackagePath, &PackageFilename))
		{
			const FDateTime FileTime = IFileManager::Get().GetTimeStamp(*PackageFilename);
			IndexedAsset.LastModified = FileTime.ToIso8601();
		}

		if (PackageHashes)
		{
			if (const FIoHash* SavedHash = PackageHashes->Find(AssetData.PackageName))
			{
				IndexedAsset.SavedHash = LexToString(*SavedHash);
			}
		}

		return IndexedAsset;
	}

	/** 自动化测试自己会创建隔离数据库，启动阶段不应再并发拉起项目自动索引。 */
	static bool ShouldSuppressStartupIndexingForAutomation()
	{
		const FString CommandLine = FCommandLine::Get();
		return CommandLine.Contains(TEXT("Automation RunTests"), ESearchCase::IgnoreCase);
	}

	/** 某些 indexer 逻辑上是“伴生数据”，不应该抢占主资产类的直接分发权。
	 *  RegisterIndexer 会把它们放进 CompanionIndexersById，
	 *  之后 live / incremental / full 路径会在主资产 materialize 完成后顺手调它们；
	 *  对 OfflineOnly companion（如 AssetVisual*），artifact pipeline 会自动 enqueue 离线 warmup queue。
	 *  漏掉任何一个 companion id，对应 cohort 在 live 路径就完全不会被触发。 */
	static bool IsCompanionIndexerId(const FName IndexerId)
	{
		return IndexerId == FName(TEXT("Dependency"))
			|| IndexerId == FName(TEXT("GameplayTags"))
			|| IndexerId == FName(TEXT("MeshCatalog"))
			|| IndexerId == FName(TEXT("AssetVisualGeometric"))
			|| IndexerId == FName(TEXT("AssetVisualSemantic"))
			|| IndexerId == FName(TEXT("GAS"));
	}

	/** 哪些 cohort 的主体数据存放在 nodes 表。 */
	static bool IsNodeShadowCohort(const FName IndexerId)
	{
		return IndexerId != FName(TEXT("Level"))
			&& IndexerId != FName(TEXT("DataTable"))
			&& IndexerId != FName(TEXT("Dependency"))
			&& IndexerId != FName(TEXT("GameplayTags"))
			&& IndexerId != FName(TEXT("MeshCatalog"))
			&& IndexerId != FName(TEXT("AssetVisualGeometric"))
			&& IndexerId != FName(TEXT("AssetVisualSemantic"));
	}

	/** 哪些 cohort 还会额外写 variables。 */
	static bool IsVariableShadowCohort(const FName IndexerId)
	{
		return IndexerId == FName(TEXT("Blueprint"))
			|| IndexerId == FName(TEXT("DataAsset"))
			|| IndexerId == FName(TEXT("UserDefinedEnum"))
			|| IndexerId == FName(TEXT("UserDefinedStruct"))
			|| IndexerId == FName(TEXT("BehaviorTree"));
	}

	/** 哪些 cohort 的 shadow diff 需要把内部 connections 也算进去。 */
	static bool IsConnectionShadowCohort(const FName IndexerId)
	{
		return IndexerId == FName(TEXT("Blueprint"))
			|| IndexerId == FName(TEXT("Material"))
			|| IndexerId == FName(TEXT("BehaviorTree"))
			|| IndexerId == FName(TEXT("EQS"))
			|| IndexerId == FName(TEXT("StateTree"));
	}

	/** Level 2 在生成最终采样 key 之前，先收集“业务主键 + row_hash”的中间形态。 */
	struct FPendingLevel2Row
	{
		FString BasePrimaryKey;
		uint64 RowHash = 0;
		FString DebugSummary;
	};

	/** 把 table 前缀和业务字段拼成稳定的 base key。 */
	static FString MakeLevel2BaseKey(const TCHAR* TableName, const TArray<FString>& Parts)
	{
		FString Result = TableName ? FString(TableName) : FString(TEXT("unknown"));
		for (const FString& Part : Parts)
		{
			Result += TEXT("|");
			Result += Part;
		}
		return Result;
	}

	/** 统一把一行塞进 Level 2 候选列表，避免各表手工拼装结构体。 */
	static void AddLevel2Row(
		TArray<FPendingLevel2Row>& Destination,
		const FString& BasePrimaryKey,
		const uint64 RowHash,
		const FString& DebugSummary = FString())
	{
		if (BasePrimaryKey.IsEmpty())
		{
			return;
		}

		FPendingLevel2Row& Row = Destination.AddDefaulted_GetRef();
		Row.BasePrimaryKey = BasePrimaryKey;
		Row.RowHash = RowHash;
		Row.DebugSummary = DebugSummary;
	}

	/*
	 * 同一个业务主键下可能会有重复行。
	 * 例如多个完全同名的 node / variable / dependency 在现实里都可能存在。
	 *
	 * 所以这里不直接拿 BasePrimaryKey 做最终 sample key，
	 * 而是先按 BasePrimaryKey + RowHash + DebugSummary 排序，再给重复项补一个稳定序号。
	 */
	static TArray<FMonolithShadowLevel2Row> FinalizeLevel2Rows(TArray<FPendingLevel2Row>&& PendingRows)
	{
		PendingRows.Sort([](const FPendingLevel2Row& A, const FPendingLevel2Row& B)
		{
			if (A.BasePrimaryKey != B.BasePrimaryKey)
			{
				return A.BasePrimaryKey < B.BasePrimaryKey;
			}

			if (A.RowHash != B.RowHash)
			{
				return A.RowHash < B.RowHash;
			}

			return A.DebugSummary < B.DebugSummary;
		});

		TArray<FMonolithShadowLevel2Row> Result;
		Result.Reserve(PendingRows.Num());

		TMap<FString, int32> OccurrenceCounts;
		for (const FPendingLevel2Row& PendingRow : PendingRows)
		{
			int32& OccurrenceIndex = OccurrenceCounts.FindOrAdd(PendingRow.BasePrimaryKey);
			FMonolithShadowLevel2Row& FinalRow = Result.AddDefaulted_GetRef();
			FinalRow.PrimaryKey = FString::Printf(TEXT("%s|dup=%d"), *PendingRow.BasePrimaryKey, OccurrenceIndex);
			FinalRow.RowHash = PendingRow.RowHash;
			FinalRow.DebugSummary = PendingRow.DebugSummary;
			++OccurrenceIndex;
		}

		return Result;
	}
}

struct FMonolithIndexConsoleCommands
{
	static FString NormalizeMode(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			return TEXT("auto");
		}

		FString Mode = Args[0];
		Mode.TrimStartAndEndInline();
		Mode.ToLowerInline();
		return Mode;
	}

	static UMonolithIndexSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	}

	static void StartIndex(const TArray<FString>& Args, UWorld* /*World*/)
	{
		UMonolithIndexSubsystem* const Subsystem = GetSubsystem();
		if (!Subsystem)
		{
			UE_LOG(LogMonolithIndex, Error, TEXT("Monolith.StartIndex failed because MonolithIndexSubsystem is unavailable"));
			return;
		}

		const FString Mode = NormalizeMode(Args);
		if (Mode == TEXT("full"))
		{
			Subsystem->RequestManualFullIndex();
			return;
		}

		if (Mode == TEXT("incremental"))
		{
			if (!Subsystem->CanDoIncrementalIndex())
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex incremental skipped because no incremental baseline is available; run 'Monolith.StartIndex full' instead"));
				return;
			}

			Subsystem->StartIncrementalIndex();
			return;
		}

		if (Mode == TEXT("auto"))
		{
			if (Subsystem->CanDoIncrementalIndex())
			{
				Subsystem->StartIncrementalIndex();
			}
			else
			{
				Subsystem->RequestManualFullIndex();
			}
			return;
		}

		UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex received unknown mode '%s'; supported modes are auto, full, incremental"), *Mode);
	}
};

namespace
{
	static FAutoConsoleCommandWithWorldAndArgs GMonolithStartIndexCommand(
		TEXT("Monolith.StartIndex"),
		TEXT("Manually start Monolith indexing. Usage: Monolith.StartIndex [auto|full|incremental]. Full indexing only runs from this manual command."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FMonolithIndexConsoleCommands::StartIndex));
}

UMonolithIndexSubsystem::UMonolithIndexSubsystem() = default;

UMonolithIndexSubsystem::~UMonolithIndexSubsystem() = default;

void FMonolithIndexRuntimeStateDeleter::operator()(FMonolithIndexRuntimeState* Ptr) const
{
	delete Ptr;
}

void FMonolithIndexSchedulerDeleter::operator()(FMonolithIndexScheduler* Ptr) const
{
	delete Ptr;
}

void UMonolithIndexSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: begin"));
	RuntimeState = MonolithIndexInternal::MakeRuntimeState();
	// 默认 cohort 集合走 MonolithIndexV2 bucket；AssetVisual 双 cohort 用各自独立的 bucket。
	ArtifactCache = MakeUnique<FMonolithDdcArtifactCache>(FString(MonolithCacheBuckets::Default));
	Scheduler = MonolithIndexInternal::MakeScheduler();
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: runtime state, cache, scheduler ready"));
	if (ArtifactCache.IsValid() && Scheduler.IsValid())
	{
		ArtifactCache->SetIoThreadPool(Scheduler->GetIoDdcThreadPool());
	}
	GtBudgetState.Reset();

	// In commandlet mode, only open the DB for queries — skip indexing, live callbacks, and AR registration
	if (IsRunningCommandlet())
	{
		const FString CommandletName = GetRunningMonolithCommandletNameFromCommandLine(FCommandLine::Get());
		if (ShouldMonolithCommandletBypassLocalSqlite(CommandletName))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("Commandlet mode — skipping local SQLite for %s"), *CommandletName);
			return;
		}

		Database = MakeUnique<FMonolithIndexDatabase>();
		FString DbPath = GetDatabasePath();
		if (Database->Open(DbPath))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("Commandlet mode — opened index DB read-only at %s"), *DbPath);
		}
		return;
	}

	Database = MakeUnique<FMonolithIndexDatabase>();
	FString DbPath = GetDatabasePath();
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: opening DB at %s"), *DbPath);

	if (!Database->Open(DbPath))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database at %s"), *DbPath);
		return;
	}
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: DB open succeeded"));

	const int32 DroppedShadowTables = Database->DropExpiredShadowTables(FDateTime::UtcNow());
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: DropExpiredShadowTables finished (%d dropped)"), DroppedShadowTables);
	if (DroppedShadowTables > 0)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Dropped %d expired Monolith shadow table(s) during startup"), DroppedShadowTables);
	}

	// 启动时一次性清理 offline warmup 队列里 IndexerId 已废弃的请求行（如 MeshVisual* 旧名字）。
	// 这些请求即使被 commandlet 消费也会被 indexer 注册表 reject，留着只会污染状态栏统计与 UI。
	{
		TArray<FMonolithOfflineWarmupRequest> ExistingQueue;
		if (LoadMonolithOfflineWarmupQueue(ExistingQueue) && ExistingQueue.Num() > 0)
		{
			static const TSet<FString> DeprecatedIndexerIds = {
				TEXT("MeshVisualGeometric"),
				TEXT("MeshVisualSemantic"),
			};
			TArray<FMonolithOfflineWarmupRequest> Survivors;
			Survivors.Reserve(ExistingQueue.Num());
			int32 Removed = 0;
			for (const FMonolithOfflineWarmupRequest& Request : ExistingQueue)
			{
				if (DeprecatedIndexerIds.Contains(Request.IndexerId))
				{
					++Removed;
					continue;
				}
				Survivors.Add(Request);
			}
			if (Removed > 0)
			{
				SaveMonolithOfflineWarmupQueue(Survivors);
				UE_LOG(LogMonolithIndex, Log,
					TEXT("Initialize: 从 offline warmup 队列里移除 %d 条 IndexerId 已废弃的请求"), Removed);
			}
		}
	}

	if (!IsIndexingEnabled())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Monolith index disabled by settings or command line — query-only mode active"));
		return;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: registering default indexers"));
	RegisterDefaultIndexers();
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: register default indexers finished"));

	if (MonolithIndexInternal::ShouldSuppressStartupIndexingForAutomation())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Automation session detected -- skipping startup indexing"));
		return;
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	UE_LOG(LogMonolithIndex, Log, TEXT("Initialize: asset registry ready, evaluating startup path"));

	if (CanDoIncrementalIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Existing index found — deferring incremental catch-up until AR ready"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::StartIncrementalIndex);
		else
			StartIncrementalIndex();
	}
	else
	{
		IndexingStatusMessage = TEXT("Full index required. Run 'Monolith.StartIndex full' from the editor console.");
		UE_LOG(LogMonolithIndex, Log, TEXT("Full index is manual-only; waiting for 'Monolith.StartIndex full'"));
	}
}

void UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded()
{
	// Unbind ourselves — this is a one-shot callback
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnFilesLoaded().RemoveAll(this);

	if (bPendingManualFullIndex)
	{
		bPendingManualFullIndex = false;
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry fully loaded -- starting requested full project index"));
		StartFullIndexInternal();
	}
	else
	{
		UE_LOG(LogMonolithIndex, Verbose, TEXT("Asset Registry files loaded with no pending manual full index request"));
	}
}

void UMonolithIndexSubsystem::Deinitialize()
{
	UnregisterLiveCallbacks();

	// Unbind from Asset Registry delegate if still bound
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
	}

	// Stop any running indexing
	if (Scheduler.IsValid())
	{
		Scheduler->RequestStop();
	}

	if (ArtifactCache.IsValid())
	{
		// 编辑器退出时，本地 SQLite 一致性优先级最高；
		// 还没真正发出去的远端写请求允许直接丢弃。
		ArtifactCache->DiscardPendingRemoteWrites();
	}

	if (IndexingTaskPtr.IsValid())
	{
		if (bIsIndexing)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing was still in progress during shutdown — force-stopped"));
		}
		IndexingTaskPtr->Stop();
	}

	if (Scheduler.IsValid())
	{
		const bool bSchedulerDrained = Scheduler->Shutdown(EMonolithSchedulerShutdownMode::AllowProcessExitAbandon);
		if (!bSchedulerDrained)
		{
			UE_LOG(
				LogMonolithIndex,
				Warning,
				TEXT("Monolith scheduler did not drain within %.2fs during shutdown; remaining thread pools will be abandoned on destruction so process exit can continue"),
				Scheduler->GetConfig().DrainTimeoutSeconds);
		}
	}

	IndexingTaskPtr.Reset();

	bIsIndexing = false;
	bPendingManualFullIndex = false;
	bRestoreLiveCallbacksAfterFullIndex = false;
	bTaskNotificationCancelRequested = false;
	if (RuntimeState.IsValid())
	{
		RuntimeState->FinishSession();
	}

	TaskNotification.Reset();

	if (Database.IsValid())
	{
		Database->Close();
	}

	Super::Deinitialize();
}

void UMonolithIndexSubsystem::RegisterIndexer(TSharedPtr<IMonolithIndexer> Indexer)
{
	if (!Indexer.IsValid()) return;

	Indexers.Add(Indexer);
	if (MonolithIndexInternal::IsCompanionIndexerId(Indexer->GetIndexerId()))
	{
		// companion indexer 也属于资产的一部分，
		// 但它们不应该覆盖“这类资产的主 indexer 到底是谁”。
		CompanionIndexersById.Add(Indexer->GetIndexerId(), Indexer);
		UE_LOG(LogMonolithIndex, Verbose, TEXT("Registered companion indexer: %s (%s)"),
			*Indexer->GetName(), *Indexer->GetIndexerId().ToString());
		return;
	}

	for (const FString& ClassName : Indexer->GetSupportedClasses())
	{
		ClassToIndexer.Add(ClassName, Indexer);
	}

	UE_LOG(LogMonolithIndex, Verbose, TEXT("Registered indexer: %s (%d classes)"),
		*Indexer->GetName(), Indexer->GetSupportedClasses().Num());
}

void UMonolithIndexSubsystem::RegisterDefaultIndexers()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();

	if (Settings->bIndexBlueprints)
		RegisterIndexer(MakeShared<FBlueprintIndexer>());
	if (Settings->bIndexMaterials)
		RegisterIndexer(MakeShared<FMaterialIndexer>());
	if (Settings->bIndexGenericAssets)
		RegisterIndexer(MakeShared<FGenericAssetIndexer>());
	if (Settings->bIndexDependencies)
		RegisterIndexer(MakeShared<FDependencyIndexer>());
	if (Settings->bIndexLevels)
		RegisterIndexer(MakeShared<FLevelIndexer>());
	if (Settings->bIndexDataTables)
		RegisterIndexer(MakeShared<FDataTableIndexer>());
	if (Settings->bIndexGameplayTags)
	{
		RegisterIndexer(MakeShared<FGameplayTagDefinitionIndexer>());
		RegisterIndexer(MakeShared<FGameplayTagIndexer>());
	}
	if (Settings->bIndexConfigs)
		RegisterIndexer(MakeShared<FConfigIndexer>());
	if (Settings->bIndexCppSymbols)
		RegisterIndexer(MakeShared<FCppIndexer>());
	if (Settings->bIndexAnimations)
		RegisterIndexer(MakeShared<FAnimationIndexer>());
	if (Settings->bIndexNiagara)
		RegisterIndexer(MakeShared<FNiagaraIndexer>());
	if (Settings->bIndexUserDefinedEnums)
		RegisterIndexer(MakeShared<FUserDefinedEnumIndexer>());
	if (Settings->bIndexUserDefinedStructs)
		RegisterIndexer(MakeShared<FUserDefinedStructIndexer>());
	if (Settings->bIndexInputActions)
		RegisterIndexer(MakeShared<FInputActionIndexer>());
	if (Settings->bIndexDataAssets)
		RegisterIndexer(MakeShared<FDataAssetIndexer>());
	if (Settings->bIndexMeshCatalog)
		RegisterIndexer(MakeShared<FMeshCatalogIndexer>());
	// AssetVisual 双 cohort 也是 StaticMesh 的 companion，与 MeshCatalog 平级独立。
	// 两边都把 provider 单例注册到全局 registry，让 indexer / search action 走同一份 Encode。
	if (Settings->bIndexAssetVisualGeometric)
	{
		FAssetVisualEmbeddingProviderRegistry::Get().RegisterProvider(MakeShared<FGeometricEmbeddingProvider>());
		RegisterIndexer(MakeShared<FAssetVisualGeometricIndexer>());
	}
	if (Settings->bIndexAssetVisualSemantic)
	{
		FAssetVisualEmbeddingProviderRegistry::Get().RegisterProvider(MakeShared<FClipSemanticEmbeddingProvider>());
		RegisterIndexer(MakeShared<FAssetVisualSemanticIndexer>());
	}
	if (Settings->bIndexGAS)
		RegisterIndexer(MakeShared<FGASIndexer>());
	if (Settings->bIndexBehaviorTrees)
	{
		RegisterIndexer(MakeShared<FBehaviorTreeIndexer>());
		RegisterIndexer(MakeShared<FEQSIndexer>());
	}
#if WITH_STATETREE
	if (Settings->bIndexStateTrees)
		RegisterIndexer(MakeShared<FStateTreeIndexer>());
#endif

	UE_LOG(LogMonolithIndex, Log, TEXT("Registered %d indexers"), Indexers.Num());
}

void UMonolithIndexSubsystem::RequestManualFullIndex()
{
	check(IsInGameThread());
	if (!IsIndexingEnabled())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("RequestManualFullIndex skipped because indexing is disabled"));
		return;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("RequestManualFullIndex skipped because indexing is already in progress"));
		return;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (AssetRegistry.IsLoadingAssets())
	{
		bPendingManualFullIndex = true;
		IndexingStatusMessage = TEXT("Full index requested; waiting for Asset Registry to finish loading...");
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
		AssetRegistry.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded);
		UE_LOG(LogMonolithIndex, Log, TEXT("Manual full index requested; waiting for Asset Registry files to finish loading"));
		return;
	}

	bPendingManualFullIndex = false;
	StartFullIndexInternal();
}

void UMonolithIndexSubsystem::StartFullIndexInternal()
{
	check(IsInGameThread());
	if (!IsIndexingEnabled())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("StartFullIndexInternal skipped because indexing is disabled"));
		return;
	}

	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing already in progress"));
		return;
	}

	bIsIndexing = true;
	bPendingManualFullIndex = false;
	bRestoreLiveCallbacksAfterFullIndex =
		OnAssetsAddedHandle.IsValid()
		|| OnAssetsRemovedHandle.IsValid()
		|| OnAssetRenamedHandle.IsValid()
		|| OnAssetsUpdatedOnDiskHandle.IsValid();
	GtBudgetState.Reset();
	bTaskNotificationCancelRequested = false;
	UnregisterLiveCallbacks();
	IndexingStatusMessage = TEXT("Running full index...");

	// Gather marketplace plugin paths for indexing
	IndexedPlugins = GatherMarketplacePluginPaths();
	const TArray<FString> IndexedPrefixes = BuildIndexedPackagePrefixes(IndexedPlugins);

	// Show notification
	if (FSlateApplication::IsInitialized())
	{
		FAsyncTaskNotificationConfig NotifConfig;
		NotifConfig.TitleText = FText::FromString(TEXT("Monolith"));
		NotifConfig.ProgressText = FText::FromString(TEXT("Indexing project..."));
		NotifConfig.bCanCancel = true;
		NotifConfig.LogCategory = &LogMonolithIndex;
		TaskNotification = MakeUnique<FAsyncTaskNotification>(NotifConfig);
	}
	else
	{
		UE_LOG(LogMonolithIndex, Verbose, TEXT("StartFullIndexInternal: skipping task notification because Slate is not initialized"));
	}

	// Launch background thread
	IndexingTaskPtr = MakeUnique<FIndexingTask>(this, GetBuildDatabasePath());
	IndexingTaskPtr->PluginsToIndex = IndexedPlugins;
	{
		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		MonolithIndexInternal::CollectManagedAssetSnapshotFromRegistry(
			AssetRegistry,
			IndexedPrefixes,
			&IndexingTaskPtr->AssetsToIndex,
			nullptr,
			&IndexingTaskPtr->PackageHashes,
			nullptr);
	}
	if (!Scheduler.IsValid())
	{
		Scheduler = MonolithIndexInternal::MakeScheduler();
		if (ArtifactCache.IsValid())
		{
			ArtifactCache->SetIoThreadPool(Scheduler->GetIoDdcThreadPool());
		}
	}

	const bool bStarted = Scheduler.IsValid() && Scheduler->StartBackgroundJob(
		[Task = IndexingTaskPtr.Get()]()
		{
			if (Task)
			{
				Task->Run();
			}
		},
		EQueuedWorkPriority::Low);

	if (!bStarted)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to start Monolith scheduler background job"));
		bIsIndexing = false;
		IndexingStatusMessage = TEXT("Full index failed to start.");
		IndexingTaskPtr.Reset();
		TaskNotification.Reset();
		if (bRestoreLiveCallbacksAfterFullIndex && IsIndexingEnabled() && Database.IsValid() && Database->IsOpen())
		{
			RegisterLiveCallbacks();
		}
		bRestoreLiveCallbacksAfterFullIndex = false;
		return;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Background full indexing started"));
}

void UMonolithIndexSubsystem::QueueTaskNotificationProgressUpdate(
	const int32 Current,
	const int32 Total,
	const FString& ProgressText)
{
	TWeakObjectPtr<UMonolithIndexSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Current, Total, ProgressText]()
	{
		UMonolithIndexSubsystem* const StrongThis = WeakThis.Get();
		if (!StrongThis)
		{
			return;
		}

		if (StrongThis->TaskNotification)
		{
			StrongThis->TaskNotification->SetProgressText(FText::FromString(ProgressText));
			if (StrongThis->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
			{
				StrongThis->bTaskNotificationCancelRequested = true;
			}
		}

		StrongThis->OnProgress.Broadcast(Current, Total);
	});
}

bool UMonolithIndexSubsystem::IsIndexing() const
{
	return bIsIndexing || (RuntimeState.IsValid() && RuntimeState->Snapshot().bIndexingInProgress);
}

float UMonolithIndexSubsystem::GetProgress() const
{
	if (RuntimeState.IsValid())
	{
		return static_cast<float>(RuntimeState->Snapshot().Progress);
	}

	if (!IndexingTaskPtr.IsValid() || IndexingTaskPtr->TotalAssets == 0)
	{
		return 0.0f;
	}

	return static_cast<float>(IndexingTaskPtr->CurrentIndex) / static_cast<float>(IndexingTaskPtr->TotalAssets);
}

// ============================================================
// Query API wrappers
// ============================================================

TArray<FSearchResult> UMonolithIndexSubsystem::Search(const FString& Query, int32 Limit)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};

	TArray<FSearchResult> Results;
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		Results = Database->FullTextSearch(Query, Limit);
	}
	for (FSearchResult& Result : Results)
	{
		Result.bStale = IsPackageStaleByMetadata(Result.AssetPath, &Result.AssetClass);
	}
	return Results;
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::FindReferences(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->FindReferences(PackagePath);
}

TArray<FIndexedAsset> UMonolithIndexSubsystem::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->FindByType(AssetClass, Limit, Offset);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetStats()
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;

	TSharedPtr<FJsonObject> Stats;
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		Stats = Database->GetStats();
	}
	if (!Stats.IsValid())
	{
		return nullptr;
	}

	const FMonolithIndexRuntimeSnapshot Snapshot = RuntimeState.IsValid()
		? RuntimeState->Snapshot()
		: FMonolithIndexRuntimeSnapshot();
	const TSet<FString> KnownStalePackages = GatherKnownStalePackages();

	Stats->SetBoolField(TEXT("indexing_in_progress"), Snapshot.bIndexingInProgress || bIsIndexing);
	Stats->SetNumberField(TEXT("progress"), Snapshot.Progress);
	Stats->SetNumberField(TEXT("completed_items"), Snapshot.CompletedItems);
	Stats->SetNumberField(TEXT("total_items"), Snapshot.TotalItems);
	Stats->SetNumberField(TEXT("queue_depth"), Snapshot.QueueDepth);
	Stats->SetNumberField(TEXT("remaining_items"), Snapshot.RemainingItems);
	Stats->SetNumberField(TEXT("eta_seconds"), Snapshot.EtaSeconds);
	Stats->SetNumberField(TEXT("stale_packages"), KnownStalePackages.Num());
	Stats->SetStringField(TEXT("status"), IndexingStatusMessage);
	AppendRuntimeStats(Stats);

	return Stats;
}

FMonolithIndexStatusBarSnapshot UMonolithIndexSubsystem::GetStatusBarSnapshot(const bool bIncludeExpensiveDetails) const
{
	FMonolithIndexStatusBarSnapshot Snapshot;
	Snapshot.bDatabaseOpen = Database.IsValid() && Database->IsOpen();
	Snapshot.bIndexEnabled = IsIndexingEnabled();
	Snapshot.bLocalCacheAvailable = ArtifactCache.IsValid();
	Snapshot.StatusMessage = IndexingStatusMessage;

	const FMonolithIndexRuntimeSnapshot RuntimeSnapshot = RuntimeState.IsValid()
		? RuntimeState->Snapshot()
		: FMonolithIndexRuntimeSnapshot();
	Snapshot.bIndexingInProgress = RuntimeSnapshot.bIndexingInProgress || bIsIndexing;
	Snapshot.Progress = RuntimeSnapshot.Progress;
	Snapshot.EtaSeconds = RuntimeSnapshot.EtaSeconds;
	Snapshot.CompletedItems = RuntimeSnapshot.CompletedItems;
	Snapshot.TotalItems = RuntimeSnapshot.TotalItems;
	Snapshot.QueueDepth = RuntimeSnapshot.QueueDepth;
	Snapshot.RemainingItems = RuntimeSnapshot.RemainingItems;

	const FMonolithArtifactCacheStats CacheStats = ArtifactCache.IsValid()
		? ArtifactCache->GetStats()
		: FMonolithArtifactCacheStats();
	Snapshot.LocalHitCount = CacheStats.LocalHitCount;
	Snapshot.RemoteHitCount = CacheStats.RemoteHitCount;
	Snapshot.RemoteMissCount = CacheStats.RemoteMissCount;
	Snapshot.RemoteWriteOkCount = CacheStats.RemoteWriteOkCount;
	Snapshot.RemoteWriteFailCount = CacheStats.RemoteWriteFailCount;
	Snapshot.RemoteWriteBytes = CacheStats.RemoteWriteBytes;
	Snapshot.OversizedArtifactCount = CacheStats.OversizedArtifactCount;
	Snapshot.PendingRemoteWriteCount = CacheStats.PendingRemoteWriteCount;
	Snapshot.InFlightRemoteWriteCount = CacheStats.InFlightRemoteWriteCount;
	Snapshot.bRemoteDisabled = CacheStats.bRemoteDisabled;
	Snapshot.RemoteBreakerRemainingSeconds = CacheStats.RemoteBreakerRemainingSeconds;

	const FMonolithIndexGtBudgetSnapshot GtSnapshot = GtBudgetState.Snapshot(FPlatformTime::Seconds());
	Snapshot.GtOverrunCount = GtSnapshot.OverrunCount;
	Snapshot.GtDowngradeCount = GtSnapshot.DowngradeCount;
	Snapshot.bGtBreakerOpen = GtSnapshot.bBreakerOpen;
	Snapshot.GtBreakerRemainingSeconds = GtSnapshot.BreakerRemainingSeconds;

	if (bIncludeExpensiveDetails)
	{
		// 这两个字段在 33K+ 资产规模下扫一次要几秒；这里走异步缓存，
		// 同步路径直接读 cache，永不阻塞 GT。第一次没数据时 UI 显示 "—"。
		{
			FScopeLock Lock(&StatusBarExpensiveCacheMutex);
			Snapshot.StalePackageCount = CachedStalePackageCount;
			Snapshot.OfflineQueueDepth = CachedOfflineQueueDepth;
		}
		KickStatusBarExpensiveRefresh();
	}

	return Snapshot;
}

void UMonolithIndexSubsystem::KickStatusBarExpensiveRefresh() const
{
	// TTL：5 秒。比 status bar 0.5s active timer 长 10 倍，
	// 既避开"每次刷新都重算"，又保证 UI 数字不会陈旧到误导。
	constexpr double RefreshTtlSeconds = 5.0;

	const double NowSeconds = FPlatformTime::Seconds();
	{
		FScopeLock Lock(&StatusBarExpensiveCacheMutex);
		if (CachedStatusBarExpensiveAtSeconds > 0.0
			&& (NowSeconds - CachedStatusBarExpensiveAtSeconds) < RefreshTtlSeconds)
		{
			return;
		}
	}

	// CAS 保证同一时刻只有一个后台刷新；并发请求直接 drop。
	bool bExpected = false;
	if (!bStatusBarExpensiveRefreshInFlight.CompareExchange(bExpected, true))
	{
		return;
	}

	UMonolithIndexSubsystem* MutableSelf = const_cast<UMonolithIndexSubsystem*>(this);
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [MutableSelf]()
	{
		// 后台线程上算两个数字：
		// 1) GatherKnownStalePackages 内部已用 DatabaseAccessMutex 串行化 SQLite 访问；
		// 2) LoadMonolithOfflineWarmupQueue 读 JSON 文件，与 GT enqueue 不冲突。
		const int32 StaleCount = MutableSelf->GatherKnownStalePackages().Num();
		TArray<FMonolithOfflineWarmupRequest> OfflineQueueRequests;
		LoadMonolithOfflineWarmupQueue(OfflineQueueRequests);
		const int32 OfflineDepth = OfflineQueueRequests.Num();

		{
			FScopeLock Lock(&MutableSelf->StatusBarExpensiveCacheMutex);
			MutableSelf->CachedStalePackageCount = StaleCount;
			MutableSelf->CachedOfflineQueueDepth = OfflineDepth;
			MutableSelf->CachedStatusBarExpensiveAtSeconds = FPlatformTime::Seconds();
		}
		MutableSelf->bStatusBarExpensiveRefreshInFlight.Store(false);
	});
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetAssetDetails(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;

	TSharedPtr<FJsonObject> Details;
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		Details = Database->GetAssetDetails(PackagePath);
	}
	if (Details.IsValid())
	{
		Details->SetBoolField(TEXT("stale"), IsPackageStaleByMetadata(PackagePath));
	}
	return Details;
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::SearchMeshCatalogBySize(
	const TArray<float>& MinBounds,
	const TArray<float>& MaxBounds,
	const FString& Category,
	const FString& ExcludeSizeClass,
	const int32 Limit)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return nullptr;
	}

	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->SearchMeshCatalogBySize(MinBounds, MaxBounds, Category, ExcludeSizeClass, Limit);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetMeshCatalogStats()
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return nullptr;
	}

	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->GetMeshCatalogStats();
}

TArray<FIndexedMeshCatalogEntry> UMonolithIndexSubsystem::GetMeshCatalogEntries(const FString& PathFilter)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return {};
	}

	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->GetMeshCatalogEntries(PathFilter);
}

TArray<FIndexedGameplayTagSummary> UMonolithIndexSubsystem::ListGameplayTags(const FString& Prefix)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return {};
	}

	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->ListGameplayTags(Prefix);
}

TArray<FIndexedGameplayTagSummary> UMonolithIndexSubsystem::SearchGameplayTags(const FString& Query)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return {};
	}

	FScopeLock Lock(&DatabaseAccessMutex);
	return Database->SearchGameplayTags(Query);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::ListStalePackages(int32 Limit, const FString& Cursor, const FString& CohortFilter)
{
	if (!RuntimeState.IsValid())
	{
		return nullptr;
	}

	// stale 包分页现在统一复用 runtime state 的 cursor 协议，
	// 避免 subsystem/action 再各自拼一份 offset + v1:* 逻辑。
	TSet<FString> StalePackages = GatherKnownStalePackages();

	// 视觉 cohort 过滤：AssetVisual* 的 stale 严格按各自表里"行不存在 / 版本三元组不对应"判定。
	// 这里用最朴素的实现：若指定 cohort 是 AssetVisual*，把不在该 cohort 表里的 mesh 也视为 stale。
	// 注：一般 stale set 已经包含 RuntimeState.IsPackageStale 与 OfflineQueue 的 mesh，
	// 视觉 cohort 单独的细化在数据库层暴露的接口已经存在；这里用 CohortFilter 收敛返回集即可。
	if (!CohortFilter.IsEmpty()
		&& (CohortFilter.Equals(TEXT("AssetVisualGeometric"), ESearchCase::IgnoreCase)
			|| CohortFilter.Equals(TEXT("AssetVisualSemantic"), ESearchCase::IgnoreCase)))
	{
		// 视觉 cohort 当前 stale 集合 = 全部 stale 包中"对应 cohort 表里没有当前 revision 行"的子集。
		TSet<FString> Filtered;
		FScopeLock Lock(&DatabaseAccessMutex);
		if (Database.IsValid() && Database->IsOpen())
		{
			for (const FString& PackagePath : StalePackages)
			{
				const TOptional<FIndexedAsset> Asset = Database->GetAssetByPath(PackagePath);
				if (!Asset.IsSet())
				{
					continue;
				}
				const TOptional<FIndexedAssetVisualEntry> Row = Database->GetAssetVisualEntryForAsset(CohortFilter, Asset->Id);
				if (!Row.IsSet())
				{
					Filtered.Add(PackagePath);
				}
			}
		}
		StalePackages = MoveTemp(Filtered);
	}

	TSharedPtr<FJsonObject> Result = FMonolithIndexRuntimeState::BuildPackagePage(StalePackages, Limit, Cursor);
	const FMonolithIndexRuntimeSnapshot Snapshot = RuntimeState->Snapshot();
	Result->SetBoolField(TEXT("indexing_in_progress"), Snapshot.bIndexingInProgress || bIsIndexing);
	if (!CohortFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("cohort_filter"), CohortFilter);
	}
	return Result;
}

TArray<FIndexedPluginInfo> UMonolithIndexSubsystem::GatherMarketplacePluginPaths() const
{
    TArray<FIndexedPluginInfo> Result;

    const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
    if (!Settings->bIndexMarketplacePlugins)
    {
        return Result;
    }

    TArray<TSharedRef<IPlugin>> ContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
    for (const TSharedRef<IPlugin>& Plugin : ContentPlugins)
    {
        // Skip engine plugins — keep project/marketplace plugins that have content directories
        if (Plugin->GetType() == EPluginType::Engine)
        {
            continue;
        }
        FString PluginContentDir = Plugin->GetContentDir();
        if (!FPaths::DirectoryExists(PluginContentDir))
        {
            continue;
        }

        FIndexedPluginInfo Info;
        Info.PluginName = Plugin->GetName();
        Info.MountPath = Plugin->GetMountedAssetPath();
        Info.ContentDir = Plugin->GetContentDir();
        Info.FriendlyName = Plugin->GetDescriptor().FriendlyName;

        UE_LOG(LogMonolithIndex, Log, TEXT("Marketplace plugin found: %s (mount: %s)"),
            *Info.FriendlyName, *Info.MountPath);

        Result.Add(MoveTemp(Info));
    }

    UE_LOG(LogMonolithIndex, Log, TEXT("Found %d marketplace plugins to index"), Result.Num());
    return Result;
}

TArray<FString> UMonolithIndexSubsystem::BuildIndexedPackagePrefixes(const TArray<FIndexedPluginInfo>& Plugins) const
{
	TArray<FString> Prefixes;
	Prefixes.Add(TEXT("/Game"));

	for (const FIndexedPluginInfo& PluginInfo : Plugins)
	{
		Prefixes.Add(PluginInfo.MountPath);
	}

	if (const UMonolithSettings* Settings = GetDefault<UMonolithSettings>())
	{
		for (const FString& CustomPath : Settings->AdditionalContentPaths)
		{
			Prefixes.Add(CustomPath);
		}
	}

	return FMonolithIndexRuntimeState::NormalizePackagePrefixes(Prefixes);
}

bool UMonolithIndexSubsystem::IsPackagePathIndexed(const FString& PackagePath) const
{
	return FMonolithIndexRuntimeState::PackageMatchesAnyPrefix(
		PackagePath,
		BuildIndexedPackagePrefixes(IndexedPlugins));
}

// ============================================================
// Background indexing task
// ============================================================

UMonolithIndexSubsystem::FIndexingTask::FIndexingTask(UMonolithIndexSubsystem* InOwner, const FString& InBuildDatabasePath)
	: BuildDatabasePath(InBuildDatabasePath)
	, Owner(InOwner)
{
}

uint32 UMonolithIndexSubsystem::FIndexingTask::Run()
{
	const UMonolithSettings* GlobalSettings = GetDefault<UMonolithSettings>();
	const bool bLogMemory = GlobalSettings ? GlobalSettings->bLogMemoryStats : true;

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index starting"));
	}

	// full index 现在只消费启动前在 GT 抓好的快照。
	// 后台线程绝不再直接触碰 Asset Registry，避免线程断言和隐式同步。
	TArray<FAssetData> AllAssets = MoveTemp(AssetsToIndex);
	TMap<FName, TArray<FAssetData>> AssetsByPackage;
	MonolithIndexInternal::BuildAssetsByPackageSnapshot(AllAssets, AssetsByPackage);

	TotalAssets = AllAssets.Num();
	if (Owner->RuntimeState.IsValid())
	{
		Owner->RuntimeState->BeginSession(MonolithIndexInternal::CollectPackagePaths(AllAssets), AllAssets.Num());
		Owner->RuntimeState->UpdateProgress(0, AllAssets.Num());
	}
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Scanning %d assets..."), TotalAssets.Load());
	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %d assets..."), TotalAssets.Load());

	constexpr double NotificationUpdateIntervalSeconds = 0.25;
	double LastNotificationUpdateSeconds = 0.0;
	auto MaybeQueueProgressUpdate =
		[this, &LastNotificationUpdateSeconds](const int32 Current, const int32 Total, const FString& Text, const bool bForce = false)
	{
		const double NowSeconds = FPlatformTime::Seconds();
		if (!bForce && LastNotificationUpdateSeconds > 0.0
			&& (NowSeconds - LastNotificationUpdateSeconds) < NotificationUpdateIntervalSeconds)
		{
			return;
		}

		LastNotificationUpdateSeconds = NowSeconds;
		Owner->QueueTaskNotificationProgressUpdate(Current, Total, Text);
	};
	MaybeQueueProgressUpdate(
		0,
		TotalAssets.Load(),
		FString::Printf(TEXT("Indexing %d / %d assets..."), 0, TotalAssets.Load()),
		true);

	TUniquePtr<FMonolithIndexDatabase> BuildDatabase = MakeUnique<FMonolithIndexDatabase>();
	IFileManager::Get().Delete(*BuildDatabasePath, false, true);
	if (!BuildDatabase->Open(BuildDatabasePath))
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			Owner->OnIndexingFinished(false);
		});
		return 1;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Full index: resetting build database at %s"), *BuildDatabasePath);
	const bool bResetOk = BuildDatabase->ResetDatabase();
	UE_LOG(
		LogMonolithIndex,
		Log,
		TEXT("Full index: build database reset %s at %s"),
		bResetOk ? TEXT("succeeded") : TEXT("failed"),
		*BuildDatabasePath);
	FMonolithIndexDatabase* DB = BuildDatabase.Get();

	DB->BeginTransaction();

	int32 BatchSize = 100;
	int32 Indexed = 0;
	int32 Errors = 0;

	// Collect assets that have deep indexers for a second pass
	struct FDeepIndexEntry
	{
		FAssetData AssetData;
		int64 AssetId;
		TSharedPtr<IMonolithIndexer> Indexer;
	};
	TArray<FDeepIndexEntry> DeepIndexQueue;
	TSet<FString> CompanionOnlyPackages;

	TMap<FString, int32> ClassDistribution;
	TMap<FString, int32> QueuedClassDistribution;

	for (int32 i = 0; i < AllAssets.Num(); ++i)
	{
		if (bShouldStop) break;

		if (Owner->bTaskNotificationCancelRequested.Load())
		{
			bShouldStop = true;
			break;
		}

		const FAssetData& AssetData = AllAssets[i];
		CurrentIndex = i + 1;

		// Insert the base asset record
		FIndexedAsset IndexedAsset = MonolithIndexInternal::BuildIndexedAssetRecord(
			AssetData,
			PluginsToIndex,
			&PackageHashes);
		ClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;

		int64 AssetId = DB->InsertAsset(IndexedAsset);
		if (AssetId < 0)
		{
			Errors++;
			continue;
		}

		// Queue assets that have deep indexers (Blueprint, Material, etc.)
		TSharedPtr<IMonolithIndexer>* FoundIndexer = Owner->ClassToIndexer.Find(IndexedAsset.AssetClass);
		if (FoundIndexer && FoundIndexer->IsValid())
		{
			DeepIndexQueue.Add({ AssetData, AssetId, *FoundIndexer });
			QueuedClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;
		}
		else
		{
			// 没有主 deep indexer 的资产，不再走旧 sentinel 旁路，
			// 而是统一记下包路径，后面交给 companion-only helper 处理。
			CompanionOnlyPackages.Add(IndexedAsset.PackagePath);
		}

		Indexed++;

		// Commit in batches
		if (Indexed % BatchSize == 0)
		{
			DB->CommitTransaction();
			DB->BeginTransaction();

			UE_LOG(LogMonolithIndex, Log, TEXT("Indexed %d / %d assets (%d errors)"),
				Indexed, TotalAssets.Load(), Errors);

			MaybeQueueProgressUpdate(
				CurrentIndex.Load(),
				TotalAssets.Load(),
				FString::Printf(TEXT("Indexing %d / %d assets..."), CurrentIndex.Load(), TotalAssets.Load()));

			if (Owner->RuntimeState.IsValid())
			{
				Owner->RuntimeState->UpdateProgress(CurrentIndex.Load(), TotalAssets.Load());
			}
		}
	}

	// Log class distribution summary
	UE_LOG(LogMonolithIndex, Log, TEXT("Asset class distribution (top 20):"));
	ClassDistribution.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 Shown = 0;
	for (const auto& Pair : ClassDistribution)
	{
		if (Shown++ >= 20) break;
		UE_LOG(LogMonolithIndex, Log, TEXT("  %s: %d"), *Pair.Key, Pair.Value);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep index queue: %d assets across %d classes"),
		DeepIndexQueue.Num(), QueuedClassDistribution.Num());
	for (const auto& Pair : QueuedClassDistribution)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("  Queued %s: %d"), *Pair.Key, Pair.Value);
	}

	DB->CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("Metadata pass complete: %d assets indexed, %d errors"), Indexed, Errors);

	// ============================================================
	// Deep indexing pass — load assets on game thread in time-budgeted batches
	// Assets must be loaded on the game thread to avoid texture compiler crashes.
	// We process in small batches with GC and memory management to prevent OOM.
	// ============================================================
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Deep indexing %d assets..."), DeepIndexQueue.Num());
	MaybeQueueProgressUpdate(
		Indexed,
		Indexed + DeepIndexQueue.Num(),
		FString::Printf(TEXT("Deep indexing %d / %d assets..."), 0, DeepIndexQueue.Num()),
		true);

	if (!bShouldStop && DeepIndexQueue.Num() > 0)
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		FMonolithMemoryHelper::LogTierStartupOnce();
		const int32 DeepBatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedDeepIndexBatchSize());
		const int32 GCFrequency = FMath::Max(1, Settings->GCFrequencyBatches);
		const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
		const float YieldTime = Settings->YieldTimeSeconds;

		UE_LOG(LogMonolithIndex, Log, TEXT("Starting deep indexing pass for %d assets (batch size: %d, GC every %d batches, memory budget: %llu MB)..."),
			DeepIndexQueue.Num(), DeepBatchSize, GCFrequency, MemoryBudgetMB);

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index start"));
		}

		TAtomic<int32> DeepIndexed{0};
		TAtomic<int32> DeepErrors{0};
		int32 TotalDeep = DeepIndexQueue.Num();
		int32 BatchNumber = 0;

		for (int32 BatchStart = 0; BatchStart < TotalDeep && !bShouldStop; BatchStart += DeepBatchSize)
		{
			// Check for cancellation from notification
			if (Owner->bTaskNotificationCancelRequested.Load())
			{
				bShouldStop = true;
				break;
			}

			// Memory budget check - throttle if over budget
			if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Memory budget exceeded, requesting async GC and yielding..."));
				FMonolithMemoryHelper::RequestGarbageCollection(true, true);
				if (YieldTime > 0.0f)
				{
					FPlatformProcess::Sleep(YieldTime);
				}

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(TEXT("After throttle GC request"));
				}
			}

			// Check for critical memory situation
			if (FMonolithMemoryHelper::IsMemoryCritical())
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("Critical memory situation detected (<2GB available). Requesting async GC and pausing indexing..."));
				FMonolithMemoryHelper::RequestGarbageCollection(true, true);
				FPlatformProcess::Sleep(1.0f);
			}

			int32 BatchEnd = FMath::Min(BatchStart + DeepBatchSize, TotalDeep);

			// Capture the slice for this batch
			TArray<FDeepIndexEntry> BatchSlice;
			BatchSlice.Reserve(BatchEnd - BatchStart);
			for (int32 j = BatchStart; j < BatchEnd; ++j)
			{
				BatchSlice.Add(DeepIndexQueue[j]);
			}

			// per-asset 正式链已经收口到 artifact materialize，
			// 编辑器后台不再现场加载 UObject，所以这里可以直接留在后台线程执行。
			// 真正需要 LoadedAsset 的构建统一转交离线 warmup commandlet。
			[DB, OwnerSubsystem = Owner, BatchSlice = MoveTemp(BatchSlice), &DeepIndexed, &DeepErrors]()
			{
				DB->BeginTransaction();

				for (const FDeepIndexEntry& Entry : BatchSlice)
				{
					bool bDeferredToOfflineWarmup = false;
					const bool bSucceeded = OwnerSubsystem->TryIndexPrimaryAssetInRevision(
						Entry.AssetData,
						Entry.Indexer,
						Entry.AssetId,
						EMonolithArtifactCacheRequestMode::Background,
						*DB,
						bDeferredToOfflineWarmup);
					if (bSucceeded)
					{
						DeepIndexed++;
					}
					else if (!bDeferredToOfflineWarmup)
					{
						DeepErrors++;
						UE_LOG(
							LogMonolithIndex,
							Warning,
							TEXT("Deep indexer '%s' failed for: %s"),
							*Entry.Indexer->GetName(),
							*Entry.AssetData.PackageName.ToString());
					}
				}

				DB->CommitTransaction();
			}();

			BatchNumber++;

			// Periodic GC based on configured frequency
			if (BatchNumber % GCFrequency == 0)
			{
				FMonolithMemoryHelper::RequestGarbageCollection(false, true);
			}

			// Update progress — report deep pass as second half of overall progress
			CurrentIndex = Indexed + BatchEnd;
			TotalAssets = Indexed + TotalDeep;

			MaybeQueueProgressUpdate(
				CurrentIndex.Load(),
				TotalAssets.Load(),
				FString::Printf(TEXT("Deep indexing %d / %d assets..."), BatchEnd, TotalDeep));

			if (Owner->RuntimeState.IsValid())
			{
				Owner->RuntimeState->UpdateProgress(CurrentIndex.Load(), TotalAssets.Load());
			}

			// Log progress and memory periodically
			if (BatchNumber % 10 == 0)
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexed %d / %d assets (%d ok, %d errors)"),
					BatchEnd, TotalDeep, DeepIndexed.Load(), DeepErrors.Load());

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("After batch %d"), BatchNumber));
				}
			}
		}

		// Final GC after deep indexing
		FMonolithMemoryHelper::RequestGarbageCollection(true, false);

		UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexing complete: %d indexed, %d errors"),
			DeepIndexed.Load(), DeepErrors.Load());

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index complete"));
		}
	}

	if (!bShouldStop && CompanionOnlyPackages.Num() > 0)
	{
		Owner->ProcessCompanionOnlyPackages(
			AssetsByPackage,
			CompanionOnlyPackages,
			EMonolithArtifactCacheRequestMode::Background,
			*DB,
			&bShouldStop);

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("After companion indexing"));
		}
	}

	// Helper to check for cancellation
	auto CheckCancellation = [this]() -> bool
	{
		if (bShouldStop) return true;
		if (Owner->bTaskNotificationCancelRequested.Load())
		{
			bShouldStop = true;
			return true;
		}
		return false;
	};

	auto GCBetweenIndexers = [bLogMemory]()
	{
		FMonolithMemoryHelper::RequestGarbageCollection(true, false);

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("After post-pass GC"));
		}
	};

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting post-pass indexers..."));

	UE_LOG(LogMonolithIndex, Log, TEXT("Level indexing already runs per asset; skipping legacy level post-pass."));
	UE_LOG(LogMonolithIndex, Log, TEXT("Dependency companion indexing already ran per asset; skipping legacy dependency post-pass."));

	/*
	 * full index 尾声里还保留着少量“不是单资产主链”的任务：
	 * - Config / Cpp 这种真正的全局扫描；
	 * - GameplayTag 定义树这种“全局定义刷新”。
	 *
	 * 以前这里每种都写一份：
	 * - 自己找 indexer；
	 * - 自己开事务；
	 * - 自己构造假 AssetData；
	 * - 自己记日志和 anonymous work。
	 *
	 * 现在统一收口成这一份 helper，避免 post-pass 再长出一堆重复分叉。
	 */
	auto RunGlobalIndexerPass = [this, &CheckCancellation, &GCBetweenIndexers, DB](
		const FName IndexerId,
		const TCHAR* StatusMessage,
		const TCHAR* LogLabel,
		const bool bRunGcAfterPass)
	{
		if (CheckCancellation())
		{
			return;
		}

		Owner->IndexingStatusMessage = StatusMessage ? StatusMessage : TEXT("");
		const TSharedPtr<IMonolithIndexer> Indexer = Owner->FindIndexerById(IndexerId);
		if (!Indexer.IsValid())
		{
			UE_LOG(LogMonolithIndex, Verbose, TEXT("Skipping %s post-pass because indexer %s is not registered"), LogLabel, *IndexerId.ToString());
			return;
		}

		if (Owner->RuntimeState.IsValid())
		{
			Owner->RuntimeState->BeginAnonymousWork();
		}

		const double PassStartSeconds = FPlatformTime::Seconds();
		bool bSucceeded = false;
		auto ExecuteGlobalPass = [DB, Indexer, OwnerSubsystem = Owner, &bSucceeded]()
		{
			DB->BeginTransaction();
			bool bUsedCachedArtifact = false;
			bSucceeded = MonolithGlobalArtifactPipeline::ExecuteGlobalIndexerArtifact(
				*Indexer,
				OwnerSubsystem->ArtifactCache.Get(),
				*DB,
				bUsedCachedArtifact);
			(void)bUsedCachedArtifact;
			if (bSucceeded)
			{
				DB->CommitTransaction();
			}
			else
			{
				DB->RollbackTransaction();
			}
		};

		UE_LOG(LogMonolithIndex, Log, TEXT("Running %s..."), LogLabel);
		ExecuteGlobalPass();

		const double DurationSeconds = FPlatformTime::Seconds() - PassStartSeconds;
		if (bSucceeded)
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("%s completed in %.2fs"), LogLabel, DurationSeconds);
		}
		else
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("%s failed after %.2fs"), LogLabel, DurationSeconds);
		}

		if (Owner->RuntimeState.IsValid())
		{
			Owner->RuntimeState->CompleteAnonymousWork();
		}

		if (bSucceeded && bRunGcAfterPass)
		{
			GCBetweenIndexers();
		}
	};

	RunGlobalIndexerPass(FName(TEXT("Config")), TEXT("Indexing config files..."), TEXT("config indexer"), false);
	RunGlobalIndexerPass(FName(TEXT("Cpp")), TEXT("Indexing C++ symbols..."), TEXT("C++ symbol indexer"), false);
	// tag 定义树现在和其它 reducer 一样走后台 artifact 主链，
	// 不再单独切回 GT 并同步等待。
	RunGlobalIndexerPass(FName(TEXT("GameplayTagDefinitions")), TEXT("Refreshing gameplay tag definitions..."), TEXT("gameplay tag definition refresh"), true);

	UE_LOG(LogMonolithIndex, Log, TEXT("Post-pass indexers complete"));

	// Write index timestamp to meta (only if not cancelled and asset count looks valid)
	if (!bShouldStop)
	{
		constexpr int32 MinAssetCountThreshold = 500;
		if (Indexed < MinAssetCountThreshold)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Index only found %d assets — Asset Registry may not have been fully loaded. Skipping last_full_index write so next launch will re-index."), Indexed);
		}
		else
		{
			DB->WriteMeta(TEXT("last_full_index"), FDateTime::UtcNow().ToString());
			UE_LOG(LogMonolithIndex, Log, TEXT("Wrote last_full_index timestamp (%d assets indexed)"), Indexed);
		}
	}

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index complete"));
	}

	BuildDatabase->Close();

	AsyncTask(ENamedThreads::GameThread, [this, CompletedDatabasePath = BuildDatabasePath]()
	{
		Owner->OnIndexingFinished(!bShouldStop, CompletedDatabasePath);
	});

	return 0;
}

void UMonolithIndexSubsystem::OnIndexingFinished(bool bSuccess, const FString& CompletedDatabasePath)
{
	bIsIndexing = false;
	bTaskNotificationCancelRequested = false;
	IndexingStatusMessage.Empty();

	IndexingTaskPtr.Reset();

	if (!CompletedDatabasePath.IsEmpty())
	{
		if (bSuccess)
		{
			const FString FinalDatabasePath = GetDatabasePath();
			const FString BackupDatabasePath = FinalDatabasePath + TEXT(".bak");
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

			FScopeLock DatabaseLock(&DatabaseAccessMutex);
			if (Database.IsValid())
			{
				Database->Close();
			}

			PlatformFile.DeleteFile(*BackupDatabasePath);

			bool bPromoteOk = true;
			if (PlatformFile.FileExists(*FinalDatabasePath))
			{
				bPromoteOk = PlatformFile.MoveFile(*BackupDatabasePath, *FinalDatabasePath);
			}

			if (bPromoteOk)
			{
				bPromoteOk = PlatformFile.MoveFile(*FinalDatabasePath, *CompletedDatabasePath);
			}

			if (!bPromoteOk)
			{
				UE_LOG(LogMonolithIndex, Error, TEXT("Failed to promote rebuilt index database from %s to %s"), *CompletedDatabasePath, *FinalDatabasePath);
				IFileManager::Get().Delete(*CompletedDatabasePath, false, true);
				if (PlatformFile.FileExists(*BackupDatabasePath) && !PlatformFile.FileExists(*FinalDatabasePath))
				{
					const bool bRestoreOk = PlatformFile.MoveFile(*FinalDatabasePath, *BackupDatabasePath);
					if (!bRestoreOk)
					{
						UE_LOG(LogMonolithIndex, Error, TEXT("Failed to restore backup index database from %s"), *BackupDatabasePath);
					}
				}
				bSuccess = false;
			}

			if (!Database.IsValid())
			{
				Database = MakeUnique<FMonolithIndexDatabase>();
			}

			if (!Database->Open(FinalDatabasePath))
			{
				UE_LOG(LogMonolithIndex, Error, TEXT("Failed to reopen promoted index database at %s"), *FinalDatabasePath);

				if (PlatformFile.FileExists(*BackupDatabasePath))
				{
					PlatformFile.DeleteFile(*FinalDatabasePath);
					if (PlatformFile.MoveFile(*FinalDatabasePath, *BackupDatabasePath) && Database->Open(FinalDatabasePath))
					{
						UE_LOG(LogMonolithIndex, Warning, TEXT("Restored backup index database after reopen failure"));
					}
					else
					{
						UE_LOG(LogMonolithIndex, Error, TEXT("Failed to restore backup database after reopen failure"));
					}
				}

				bSuccess = false;
			}
			else
			{
				PlatformFile.DeleteFile(*BackupDatabasePath);
			}
		}
		else
		{
			IFileManager::Get().Delete(*CompletedDatabasePath, false, true);
		}
	}

	if (RuntimeState.IsValid())
	{
		RuntimeState->FinishSession();
	}

	if ((bSuccess || bRestoreLiveCallbacksAfterFullIndex) && IsIndexingEnabled() && Database.IsValid() && Database->IsOpen())
	{
		RegisterLiveCallbacks();
	}
	bRestoreLiveCallbacksAfterFullIndex = false;

	if (TaskNotification)
	{
		TaskNotification->SetComplete(
			FText::FromString(TEXT("Monolith")),
			FText::FromString(bSuccess ? TEXT("Project indexing complete") : TEXT("Project indexing failed")),
			bSuccess);
		TaskNotification.Reset();
	}

	OnComplete.Broadcast(bSuccess);
	OnProgress.Clear();

	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %s"),
		bSuccess ? TEXT("completed successfully") : TEXT("failed or was cancelled"));
}

FMonolithActionResult UMonolithIndexSubsystem::RunReadDatabaseAction(
	TFunctionRef<FMonolithActionResult(FMonolithIndexDatabase&)> Func)
{
	/*
	 * 这里把所有 project.* 查询统一收口到“主数据库连接 + DatabaseAccessMutex”。
	 *
	 * 之前我们单独维护了一个 ReadDatabase，想把查询和写入拆成两份 sqlite3 连接。
	 * 但真实验证已经证明：在这个工程环境里，“同进程第二个连接”本身就会反复报
	 * disk I/O error，哪怕主连接在同一时刻是健康可用的。
	 *
	 * 所以这里回到唯一正确实现：
	 * - 查询直接复用启动时已经打开成功的主连接；
	 * - 查询和增量/live 写入统一串在 DatabaseAccessMutex 上；
	 * - 不再保留第二个“看似只读、实际上更脆弱”的连接分支。
	 */
	FScopeLock Lock(&DatabaseAccessMutex);
	if (!Database.IsValid() || !Database->IsOpen())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Index database not available: %s"), *GetDatabasePath()));
	}

	return Func(*Database);
}

FString UMonolithIndexSubsystem::GetDatabasePath() const
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (Settings && !Settings->DatabasePathOverride.Path.IsEmpty())
	{
		FString OverridePath = Settings->DatabasePathOverride.Path;
		OverridePath = FPaths::ConvertRelativePathToFull(OverridePath);
		if (FPaths::GetExtension(OverridePath).Equals(TEXT("db"), ESearchCase::IgnoreCase))
		{
			return OverridePath;
		}
		return OverridePath / TEXT("ProjectIndex.db");
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("ProjectIndex.db");
	}
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("ProjectIndex.db");
}

FString UMonolithIndexSubsystem::GetBuildDatabasePath() const
{
	return MonolithIndexInternal::MakeBuildDatabasePath(GetDatabasePath());
}

bool UMonolithIndexSubsystem::ShouldAutoIndex() const
{
	if (!Database.IsValid() || !Database->IsOpen()) return false;
	if (const UMonolithSettings* Settings = GetDefault<UMonolithSettings>())
	{
		if (Settings->bDeferFirstTimeIndex)
		{
			return false;
		}
	}

	if (!Database->ReadMeta(TEXT("last_full_index")).IsEmpty())
	{
		return false;
	}

	// 某些旧库或异常退出场景里，资产快照已经写好了，但“上次 full index 完成”的时间戳没写上。
	// 这时如果继续按“首次启动”处理，会把本来可以直接增量恢复的库误判成必须 full index。
	if (Database->HasIndexedAssetSnapshot())
	{
		UE_LOG(
			LogMonolithIndex,
			Warning,
			TEXT("ShouldAutoIndex: found indexed asset snapshot but last_full_index is missing; treating database as already initialized"));
		return false;
	}

	return true;
}

bool UMonolithIndexSubsystem::IsIndexingEnabled() const
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const bool bEnabledBySettings = !Settings || Settings->bEnableIndex;
	const bool bDisabledByCommandLine = FParse::Param(FCommandLine::Get(), TEXT("nomonolithindex"));
	return bEnabledBySettings && !bDisabledByCommandLine;
}

bool UMonolithIndexSubsystem::TryGetShadowModeCohortName(FString& OutCohortName) const
{
	OutCohortName.Reset();

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableArtifactShadowMode)
	{
		return false;
	}

	FName CohortName;
	if (!ParseShadowModeCohort(Settings->ShadowModeCohort, CohortName))
	{
		return false;
	}

	OutCohortName = CohortName.ToString();
	return !OutCohortName.IsEmpty();
}

bool UMonolithIndexSubsystem::ShouldWriteArtifactShadowForIndexer(
	const IMonolithIndexer& Indexer,
	FString& OutCohortName) const
{
	if (!ArtifactCache.IsValid())
	{
		OutCohortName.Reset();
		return false;
	}

	if (!TryGetShadowModeCohortName(OutCohortName))
	{
		return false;
	}

	return OutCohortName.Equals(Indexer.GetIndexerId().ToString(), ESearchCase::IgnoreCase)
		|| OutCohortName.Equals(Indexer.GetName(), ESearchCase::IgnoreCase);
}

void UMonolithIndexSubsystem::EvaluateShadowArtifactDiff(
	FMonolithIndexDatabase& DB,
	const int64 AssetId,
	const IMonolithIndexer& Indexer) const
{
	FString CohortName;
	if (!TryGetShadowModeCohortName(CohortName))
	{
		return;
	}

	FMonolithShadowAggregate Production;
	FMonolithShadowAggregate Shadow;

	const FName IndexerId = Indexer.GetIndexerId();
	const bool bLevelCohort = IndexerId == FName(TEXT("Level"));
	const bool bDataTableCohort = IndexerId == FName(TEXT("DataTable"));
	const bool bDependencyCohort = IndexerId == FName(TEXT("Dependency"));
	const bool bGameplayTagCohort = IndexerId == FName(TEXT("GameplayTags"));
	const bool bMeshCatalogCohort = IndexerId == FName(TEXT("MeshCatalog"));
	const bool bMaterialCohort = IndexerId == FName(TEXT("Material"));

	// 各 cohort 到底要比较哪些基础表，现在统一收口到上面的 helper。
	// 这样后面再迁新的图类资产时，只要声明“它属于哪一类 shadow 语义”就够了。
	const bool bNodeCohort = MonolithIndexInternal::IsNodeShadowCohort(IndexerId);
	const bool bVariableCohort = MonolithIndexInternal::IsVariableShadowCohort(IndexerId);
	const bool bConnectionCohort = MonolithIndexInternal::IsConnectionShadowCohort(IndexerId);
	if (bNodeCohort)
	{
		const FMonolithShadowNodeAggregate ProductionNodeAggregate = DB.GetProductionNodeAggregateForAsset(AssetId);
		const FMonolithShadowNodeAggregate ShadowNodeAggregate = DB.GetShadowNodeAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionNodeAggregate.RowCount, ProductionNodeAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowNodeAggregate.RowCount, ShadowNodeAggregate.RowHashSum);
	}
	if (bVariableCohort)
	{
		const FMonolithShadowVariableAggregate ProductionVariableAggregate = DB.GetProductionVariableAggregateForAsset(AssetId);
		const FMonolithShadowVariableAggregate ShadowVariableAggregate = DB.GetShadowVariableAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionVariableAggregate.RowCount, ProductionVariableAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowVariableAggregate.RowCount, ShadowVariableAggregate.RowHashSum);
	}
	if (bConnectionCohort)
	{
		const FMonolithShadowConnectionAggregate ProductionConnectionAggregate = DB.GetProductionConnectionAggregateForAsset(AssetId);
		const FMonolithShadowConnectionAggregate ShadowConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionConnectionAggregate.RowCount, ProductionConnectionAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowConnectionAggregate.RowCount, ShadowConnectionAggregate.RowHashSum);
	}
	if (bLevelCohort)
	{
		const FMonolithShadowActorAggregate ProductionActorAggregate = DB.GetProductionActorAggregateForAsset(AssetId);
		const FMonolithShadowActorAggregate ShadowActorAggregate = DB.GetShadowActorAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionActorAggregate.RowCount, ProductionActorAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowActorAggregate.RowCount, ShadowActorAggregate.RowHashSum);
	}
	if (bDataTableCohort)
	{
		const FMonolithShadowDataTableRowAggregate ProductionDataTableAggregate = DB.GetProductionDataTableRowAggregateForAsset(AssetId);
		const FMonolithShadowDataTableRowAggregate ShadowDataTableAggregate = DB.GetShadowDataTableRowAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionDataTableAggregate.RowCount, ProductionDataTableAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowDataTableAggregate.RowCount, ShadowDataTableAggregate.RowHashSum);
	}
	if (bDependencyCohort)
	{
		const FMonolithShadowDependencyAggregate ProductionDependencyAggregate = DB.GetProductionDependencyAggregateForAsset(AssetId);
		const FMonolithShadowDependencyAggregate ShadowDependencyAggregate = DB.GetShadowDependencyAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionDependencyAggregate.RowCount, ProductionDependencyAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowDependencyAggregate.RowCount, ShadowDependencyAggregate.RowHashSum);
	}
	if (bGameplayTagCohort)
	{
		const FMonolithShadowTagReferenceAggregate ProductionTagAggregate = DB.GetProductionTagReferenceAggregateForAsset(AssetId);
		const FMonolithShadowTagReferenceAggregate ShadowTagAggregate = DB.GetShadowTagReferenceAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionTagAggregate.RowCount, ProductionTagAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowTagAggregate.RowCount, ShadowTagAggregate.RowHashSum);
	}
	if (bMeshCatalogCohort)
	{
		const FMonolithShadowMeshCatalogAggregate ProductionMeshCatalogAggregate = DB.GetProductionMeshCatalogAggregateForAsset(AssetId);
		const FMonolithShadowMeshCatalogAggregate ShadowMeshCatalogAggregate = DB.GetShadowMeshCatalogAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionMeshCatalogAggregate.RowCount, ProductionMeshCatalogAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowMeshCatalogAggregate.RowCount, ShadowMeshCatalogAggregate.RowHashSum);
	}
	if (bMaterialCohort)
	{
		const FMonolithShadowParameterAggregate ProductionParameterAggregate = DB.GetProductionParameterAggregateForAsset(AssetId);
		const FMonolithShadowParameterAggregate ShadowParameterAggregate = DB.GetShadowParameterAggregateForAsset(CohortName, AssetId);
		MonolithIndexInternal::AccumulateShadowAggregate(Production, ProductionParameterAggregate.RowCount, ProductionParameterAggregate.RowHashSum);
		MonolithIndexInternal::AccumulateShadowAggregate(Shadow, ShadowParameterAggregate.RowCount, ShadowParameterAggregate.RowHashSum);
	}

	const FMonolithShadowDiffDecision Decision = EvaluateShadowDiff(Production, Shadow);

	FMonolithShadowLevel2DiffResult Level2Result;
	if (Decision.bRequiresLevel2)
	{
		TArray<MonolithIndexInternal::FPendingLevel2Row> ProductionLevel2Rows;
		TArray<MonolithIndexInternal::FPendingLevel2Row> ShadowLevel2Rows;

		if (bNodeCohort)
		{
			for (const FIndexedNode& Node : DB.GetNodesForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Node.NodeType);
				KeyParts.Add(Node.NodeName);
				KeyParts.Add(Node.NodeClass);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("nodes"), KeyParts),
					ComputeNodeRowHash(Node),
					Node.Properties);
			}

			for (const FMonolithShadowIndexedNode& ShadowNode : DB.GetShadowNodesForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowNode.Node.NodeType);
				KeyParts.Add(ShadowNode.Node.NodeName);
				KeyParts.Add(ShadowNode.Node.NodeClass);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("nodes"), KeyParts),
					ShadowNode.RowHash,
					ShadowNode.Node.Properties);
			}
		}

		if (bVariableCohort)
		{
			for (const FIndexedVariable& Variable : DB.GetVariablesForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Variable.VarName);
				KeyParts.Add(Variable.VarType);
				KeyParts.Add(Variable.Category);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("variables"), KeyParts),
					ComputeVariableRowHash(Variable),
					Variable.DefaultValue);
			}

			for (const FMonolithShadowIndexedVariable& ShadowVariable : DB.GetShadowVariablesForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowVariable.Variable.VarName);
				KeyParts.Add(ShadowVariable.Variable.VarType);
				KeyParts.Add(ShadowVariable.Variable.Category);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("variables"), KeyParts),
					ShadowVariable.RowHash,
					ShadowVariable.Variable.DefaultValue);
			}
		}

		if (bConnectionCohort)
		{
			for (const FMonolithShadowIndexedConnection& Connection : DB.GetProductionConnectionsForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(LexToString(Connection.SourceNodeRowHash));
				KeyParts.Add(Connection.SourcePin);
				KeyParts.Add(LexToString(Connection.TargetNodeRowHash));
				KeyParts.Add(Connection.TargetPin);
				KeyParts.Add(Connection.PinType);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("connections"), KeyParts),
					Connection.RowHash);
			}

			for (const FMonolithShadowIndexedConnection& ShadowConnection : DB.GetShadowConnectionsForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(LexToString(ShadowConnection.SourceNodeRowHash));
				KeyParts.Add(ShadowConnection.SourcePin);
				KeyParts.Add(LexToString(ShadowConnection.TargetNodeRowHash));
				KeyParts.Add(ShadowConnection.TargetPin);
				KeyParts.Add(ShadowConnection.PinType);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("connections"), KeyParts),
					ShadowConnection.RowHash);
			}
		}

		if (bLevelCohort)
		{
			for (const FIndexedActor& Actor : DB.GetActorsForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Actor.ActorName);
				KeyParts.Add(Actor.ActorClass);
				KeyParts.Add(Actor.ActorLabel);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("actors"), KeyParts),
					ComputeActorRowHash(Actor),
					Actor.Transform);
			}

			for (const FMonolithShadowIndexedActor& ShadowActor : DB.GetShadowActorsForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowActor.Actor.ActorName);
				KeyParts.Add(ShadowActor.Actor.ActorClass);
				KeyParts.Add(ShadowActor.Actor.ActorLabel);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("actors"), KeyParts),
					ShadowActor.RowHash,
					ShadowActor.Actor.Transform);
			}
		}

		if (bDataTableCohort)
		{
			for (const FIndexedDataTableRow& Row : DB.GetDataTableRowsForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Row.RowName);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("datatable_rows"), KeyParts),
					ComputeDataTableRowHash(Row),
					Row.RowData);
			}

			for (const FMonolithShadowIndexedDataTableRow& ShadowRow : DB.GetShadowDataTableRowsForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowRow.Row.RowName);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("datatable_rows"), KeyParts),
					ShadowRow.RowHash,
					ShadowRow.Row.RowData);
			}
		}

		if (bDependencyCohort)
		{
			for (const FMonolithShadowIndexedDependency& Dependency : DB.GetProductionDependenciesForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Dependency.TargetPackagePath);
				KeyParts.Add(Dependency.DependencyType);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("dependencies"), KeyParts),
					Dependency.RowHash);
			}

			for (const FMonolithShadowIndexedDependency& ShadowDependency : DB.GetShadowDependenciesForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowDependency.TargetPackagePath);
				KeyParts.Add(ShadowDependency.DependencyType);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("dependencies"), KeyParts),
					ShadowDependency.RowHash);
			}
		}

		if (bGameplayTagCohort)
		{
			for (const FMonolithShadowIndexedTagReference& TagReference : DB.GetProductionTagReferencesForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(TagReference.TagName);
				KeyParts.Add(TagReference.Context);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("tag_references"), KeyParts),
					TagReference.RowHash);
			}

			for (const FMonolithShadowIndexedTagReference& ShadowTagReference : DB.GetShadowTagReferencesForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowTagReference.TagName);
				KeyParts.Add(ShadowTagReference.Context);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("tag_references"), KeyParts),
					ShadowTagReference.RowHash);
			}
		}

		if (bMeshCatalogCohort)
		{
			if (const TOptional<FIndexedMeshCatalogEntry> Entry = DB.GetMeshCatalogEntryForAsset(AssetId); Entry.IsSet())
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Entry->AssetPath);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("mesh_catalog"), KeyParts),
					ComputeMeshCatalogRowHash(Entry.GetValue()),
					Entry->Category);
			}

			for (const FMonolithShadowIndexedMeshCatalogEntry& ShadowEntry : DB.GetShadowMeshCatalogEntriesForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowEntry.Entry.AssetPath);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("mesh_catalog"), KeyParts),
					ShadowEntry.RowHash,
					ShadowEntry.Entry.Category);
			}
		}

		if (bMaterialCohort)
		{
			for (const FIndexedParameter& Parameter : DB.GetParametersForAsset(AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(Parameter.ParamName);
				KeyParts.Add(Parameter.ParamType);
				KeyParts.Add(Parameter.ParamGroup);
				KeyParts.Add(Parameter.Source);
				MonolithIndexInternal::AddLevel2Row(
					ProductionLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("parameters"), KeyParts),
					ComputeParameterRowHash(Parameter),
					Parameter.DefaultValue);
			}

			for (const FMonolithShadowIndexedParameter& ShadowParameter : DB.GetShadowParametersForAsset(CohortName, AssetId))
			{
				TArray<FString> KeyParts;
				KeyParts.Add(ShadowParameter.Parameter.ParamName);
				KeyParts.Add(ShadowParameter.Parameter.ParamType);
				KeyParts.Add(ShadowParameter.Parameter.ParamGroup);
				KeyParts.Add(ShadowParameter.Parameter.Source);
				MonolithIndexInternal::AddLevel2Row(
					ShadowLevel2Rows,
					MonolithIndexInternal::MakeLevel2BaseKey(TEXT("parameters"), KeyParts),
					ShadowParameter.RowHash,
					ShadowParameter.Parameter.DefaultValue);
			}
		}

		Level2Result = EvaluateShadowLevel2Diff(
			MonolithIndexInternal::FinalizeLevel2Rows(MoveTemp(ProductionLevel2Rows)),
			MonolithIndexInternal::FinalizeLevel2Rows(MoveTemp(ShadowLevel2Rows)));
	}

	const auto UpsertShadowRetention = [&DB, &CohortName, &Decision](const TCHAR* BaseTableName)
	{
		DB.UpsertShadowTableRetention(
			CohortName,
			BaseTableName,
			GetShadowRetentionDeadlineUtc(FDateTime::UtcNow(), Decision.bShouldRollback),
			Decision.bShouldRollback);
	};

	if (bNodeCohort)
	{
		UpsertShadowRetention(TEXT("nodes"));
	}
	if (bVariableCohort)
	{
		UpsertShadowRetention(TEXT("variables"));
	}
	if (bConnectionCohort)
	{
		UpsertShadowRetention(TEXT("connections"));
	}
	if (bLevelCohort)
	{
		UpsertShadowRetention(TEXT("actors"));
	}
	if (bDataTableCohort)
	{
		UpsertShadowRetention(TEXT("datatable_rows"));
	}
	if (bDependencyCohort)
	{
		UpsertShadowRetention(TEXT("dependencies"));
	}
	if (bGameplayTagCohort)
	{
		UpsertShadowRetention(TEXT("tag_references"));
	}
	if (bMeshCatalogCohort)
	{
		UpsertShadowRetention(TEXT("mesh_catalog"));
	}
	if (bMaterialCohort)
	{
		UpsertShadowRetention(TEXT("parameters"));
	}

	if (Decision.bShouldRollback || Level2Result.HasMismatch())
	{
		UE_LOG(
			LogMonolithIndex,
			Warning,
			TEXT("Shadow diff for %s (asset_id=%lld, cohort=%s): row_count=%llu/%llu row_hash_sum=%llu/%llu level1_ratio=%.4f requires_level2=%s rollback=%s level2_compared=%u level2_mismatch=%u prod_only=%u shadow_only=%u"),
			*Indexer.GetIndexerId().ToString(),
			AssetId,
			*CohortName,
			static_cast<unsigned long long>(Production.RowCount),
			static_cast<unsigned long long>(Shadow.RowCount),
			static_cast<unsigned long long>(Production.RowHashSum),
			static_cast<unsigned long long>(Shadow.RowHashSum),
			Decision.AggregateDifferenceRatio,
			Decision.bRequiresLevel2 ? TEXT("true") : TEXT("false"),
			Decision.bShouldRollback ? TEXT("true") : TEXT("false"),
			Level2Result.ComparedRows,
			Level2Result.MismatchedRows,
			Level2Result.ProductionOnlyRows,
			Level2Result.ShadowOnlyRows);
	}
	else
	{
		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("Shadow diff for %s (asset_id=%lld, cohort=%s): row_count=%llu/%llu row_hash_sum=%llu/%llu level1_ratio=%.4f requires_level2=%s rollback=%s level2_compared=%u level2_mismatch=%u prod_only=%u shadow_only=%u"),
			*Indexer.GetIndexerId().ToString(),
			AssetId,
			*CohortName,
			static_cast<unsigned long long>(Production.RowCount),
			static_cast<unsigned long long>(Shadow.RowCount),
			static_cast<unsigned long long>(Production.RowHashSum),
			static_cast<unsigned long long>(Shadow.RowHashSum),
			Decision.AggregateDifferenceRatio,
			Decision.bRequiresLevel2 ? TEXT("true") : TEXT("false"),
			Decision.bShouldRollback ? TEXT("true") : TEXT("false"),
			Level2Result.ComparedRows,
			Level2Result.MismatchedRows,
			Level2Result.ProductionOnlyRows,
			Level2Result.ShadowOnlyRows);
	}
}

bool UMonolithIndexSubsystem::TryIndexPrimaryAssetInRevision(
	const FAssetData& AssetData,
	const TSharedPtr<IMonolithIndexer>& Indexer,
	const int64 AssetId,
	const EMonolithArtifactCacheRequestMode RequestMode,
	FMonolithIndexDatabase& DB,
	bool& bOutDeferredToOfflineWarmup)
{
	bOutDeferredToOfflineWarmup = false;
	if (!Indexer.IsValid() || AssetId <= 0)
	{
		return false;
	}

	// 这条 helper 负责把“一次主资产 revision 写入”完整包起来，
	// 保证 full / incremental / live 三条链都走完全一致的顺序：
	// 1. Begin revision
	// 2. artifact 命中或按规则触发离线 warmup
	// 3. 正式生产写入
	// 4. companion 写入
	// 5. commit + diff + metadata
	if (!DB.BeginAssetRevisionWrite(AssetId))
	{
		return false;
	}

	bool bShouldDiscardRevision = true;
	ON_SCOPE_EXIT
	{
		if (bShouldDiscardRevision)
		{
			DB.DiscardAssetRevisionWrite(AssetId);
		}
	};

	TOptional<FMonolithArtifactIdentityV1> MaterializedIdentity;
	bool bShadowWritten = false;

	const bool bNeedsLoadedAsset = Indexer->GetExecutionMode() != EMonolithExecutionMode::AROnly;
	FString ShadowCohortName;
	MonolithAssetArtifactPipeline::FExecuteAssetOptions PrimaryArtifactOptions;
	PrimaryArtifactOptions.RequestMode = RequestMode;
	PrimaryArtifactOptions.bAllowLocalArtifactBuild = !bNeedsLoadedAsset;
	PrimaryArtifactOptions.bMaterializeProduction = true;
	PrimaryArtifactOptions.AssetId = AssetId;
	if (ShouldWriteArtifactShadowForIndexer(*Indexer, ShadowCohortName))
	{
		PrimaryArtifactOptions.ShadowCohortName = ShadowCohortName;
	}

	MonolithAssetArtifactPipeline::FExecuteAssetResult PrimaryArtifactResult;
	const MonolithAssetArtifactPipeline::EExecuteAssetOutcome PrimaryArtifactOutcome =
		MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
			AssetData,
			nullptr,
			{},
			*Indexer,
			ArtifactCache.Get(),
			&DB,
			PrimaryArtifactOptions,
			PrimaryArtifactResult);

	if (PrimaryArtifactOutcome == MonolithAssetArtifactPipeline::EExecuteAssetOutcome::NeedsLocalBuild)
	{
		// 编辑器侧对“必须加载 UObject 才能构建 artifact”的 indexer 只做一件事：
		// 把任务转交给离线 warmup，让查询先以 stale 语义继续服务。
		bOutDeferredToOfflineWarmup = QueueOfflineWarmupRequest(
			AssetData,
			*Indexer,
			TEXT("artifact_cache_miss_requires_warmup"));
		return false;
	}

	if (PrimaryArtifactOutcome != MonolithAssetArtifactPipeline::EExecuteAssetOutcome::Succeeded
		|| !PrimaryArtifactResult.bMaterializedProduction)
	{
		return false;
	}

	MaterializedIdentity = PrimaryArtifactResult.Identity;
	bShadowWritten = PrimaryArtifactResult.bMaterializedShadow;

	TArray<TSharedPtr<IMonolithIndexer>> CompanionShadowDiffIndexers;
	if (!RunPerAssetCompanionIndexers(
		AssetData,
		AssetId,
		RequestMode,
		DB,
		&CompanionShadowDiffIndexers))
	{
		return false;
	}

	if (!DB.CommitAssetRevisionWrite(AssetId))
	{
		return false;
	}

	bShouldDiscardRevision = false;
	if (bShadowWritten)
	{
		EvaluateShadowArtifactDiff(DB, AssetId, *Indexer);
	}
	for (const TSharedPtr<IMonolithIndexer>& CompanionShadowIndexer : CompanionShadowDiffIndexers)
	{
		if (CompanionShadowIndexer.IsValid())
		{
			EvaluateShadowArtifactDiff(DB, AssetId, *CompanionShadowIndexer);
		}
	}

	RecordSuccessfulAssetIndex(
		DB,
		AssetId,
		AssetData.PackageName.ToString(),
		*Indexer,
		MaterializedIdentity.IsSet() ? &MaterializedIdentity.GetValue() : nullptr);
	return true;
}

bool UMonolithIndexSubsystem::TryIndexCompanionOnlyAssetInRevision(
	const FAssetData& AssetData,
	const int64 AssetId,
	const EMonolithArtifactCacheRequestMode RequestMode,
	FMonolithIndexDatabase& DB)
{
	if (AssetId <= 0)
	{
		return false;
	}

	if (!DB.BeginAssetRevisionWrite(AssetId))
	{
		return false;
	}

	bool bShouldDiscardRevision = true;
	ON_SCOPE_EXIT
	{
		if (bShouldDiscardRevision)
		{
			DB.DiscardAssetRevisionWrite(AssetId);
		}
	};

	TArray<TSharedPtr<IMonolithIndexer>> CompanionShadowDiffIndexers;
	if (!RunPerAssetCompanionIndexers(
		AssetData,
		AssetId,
		RequestMode,
		DB,
		&CompanionShadowDiffIndexers))
	{
		return false;
	}

	if (!DB.CommitAssetRevisionWrite(AssetId))
	{
		return false;
	}

	bShouldDiscardRevision = false;
	for (const TSharedPtr<IMonolithIndexer>& CompanionShadowIndexer : CompanionShadowDiffIndexers)
	{
		if (CompanionShadowIndexer.IsValid())
		{
			EvaluateShadowArtifactDiff(DB, AssetId, *CompanionShadowIndexer);
		}
	}

	return true;
}

TSharedPtr<IMonolithIndexer> UMonolithIndexSubsystem::FindIndexerById(const FName IndexerId) const
{
	if (const TSharedPtr<IMonolithIndexer>* CompanionIndexer = CompanionIndexersById.Find(IndexerId))
	{
		return *CompanionIndexer;
	}

	for (const TSharedPtr<IMonolithIndexer>& Indexer : Indexers)
	{
		if (Indexer.IsValid() && Indexer->GetIndexerId() == IndexerId)
		{
			return Indexer;
		}
	}

	return nullptr;
}

void UMonolithIndexSubsystem::RecordSuccessfulAssetIndex(
	FMonolithIndexDatabase& TargetDatabase,
	const int64 AssetId,
	const FString& PackagePath,
	const IMonolithIndexer& Indexer,
	const FMonolithArtifactIdentityV1* Identity)
{
	if (!TargetDatabase.IsOpen() || AssetId <= 0)
	{
		return;
	}

	FMonolithAssetIndexMetadata Metadata;
	Metadata.AssetId = AssetId;
	Metadata.IndexerId = Indexer.GetIndexerId().ToString();
	Metadata.IndexerVersion = Indexer.GetIndexerVersion();
	Metadata.ArtifactSchemaVersion = Indexer.GetArtifactSchemaVersion();
	Metadata.ExecutionMode = LexToString(Indexer.GetExecutionMode());

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	Metadata.IdentityProvider = Identity
		? LexToString(Identity->IdentityProvider)
		: (Settings ? Settings->IndexIdentityProvider : FString(TEXT("SavedHash")));
	Metadata.IdentityHash = Identity ? LexToString(HashMonolithArtifactIdentity(*Identity)) : FString();

	TargetDatabase.UpsertAssetIndexMetadata(Metadata);

	if (!PackagePath.IsEmpty())
	{
		FMonolithOfflineWarmupRequest CompletedRequest;
		CompletedRequest.PackagePath = PackagePath;
		CompletedRequest.IndexerId = Indexer.GetIndexerId().ToString();
		TArray<FMonolithOfflineWarmupRequest> CompletedRequests;
		CompletedRequests.Add(MoveTemp(CompletedRequest));
		RemoveMonolithOfflineWarmupRequests(CompletedRequests);
	}
}

bool UMonolithIndexSubsystem::RunPerAssetCompanionIndexers(
	const FAssetData& AssetData,
	const int64 AssetId,
	const EMonolithArtifactCacheRequestMode RequestMode,
	FMonolithIndexDatabase& DB,
	TArray<TSharedPtr<IMonolithIndexer>>* OutShadowDiffIndexers)
{
	// “Companion indexer” 的意思是：
	// 某些数据虽然逻辑上属于一份资产，但历史上是放在全局 sentinel 里统一扫的。
	// 为了让 live/full 路径也能做到 per-asset revision 可见性，
	// 我们在主资产完成深度索引后，顺手再把依赖、GameplayTag、MeshCatalog 这类伴生数据补进去。
	// 这样一来：
	// 1. 有 deep index 的资产可以和 companion 数据一起 promote；
	// 2. 没有 deep index 的资产则交给统一的 companion-only helper 补写；
	// 3. 查询侧不会看到“主节点换新了，但依赖关系还是旧的”这种半新半旧状态。
	//
	// 这一层现在还额外承担一件事：
	// “只让真正命中的 companion 跑起来”。
	// 也就是说：
	// - Dependency / GameplayTags 这种全资产 companion，会对任意 package 命中；
	// - MeshCatalog 只会在 StaticMesh 上命中；
	// - GAS 只会在继承 GAS 基类的 Blueprint 上命中。
	if (AssetId <= 0 || AssetData.PackageName.IsNone())
	{
		return false;
	}

	auto RunCompanionIndexer = [
		this,
		&AssetData,
		&DB,
		AssetId,
		RequestMode,
		OutShadowDiffIndexers](const TSharedPtr<IMonolithIndexer>& CompanionIndexer) -> bool
	{
		if (!CompanionIndexer.IsValid())
		{
			return true;
		}

		// 先用 AssetData 做一次轻量筛选。
		// 这样像 MeshCatalog/GAS 这类“只命中特定资产”的 companion，
		// 就不会对所有资产都走后面的 artifact/materialize 主链。
		if (!CompanionIndexer->MatchesAsset(AssetData))
		{
			return true;
		}

		const bool bNeedsLoadedAsset = CompanionIndexer->GetExecutionMode() != EMonolithExecutionMode::AROnly;
		FString ShadowCohortName;
		MonolithAssetArtifactPipeline::FExecuteAssetOptions CompanionArtifactOptions;
		CompanionArtifactOptions.RequestMode = RequestMode;
		CompanionArtifactOptions.bAllowLocalArtifactBuild = !bNeedsLoadedAsset;
		CompanionArtifactOptions.bMaterializeProduction = true;
		CompanionArtifactOptions.AssetId = AssetId;
		if (ShouldWriteArtifactShadowForIndexer(*CompanionIndexer, ShadowCohortName))
		{
			CompanionArtifactOptions.ShadowCohortName = ShadowCohortName;
		}

		MonolithAssetArtifactPipeline::FExecuteAssetResult CompanionArtifactResult;
		const MonolithAssetArtifactPipeline::EExecuteAssetOutcome CompanionArtifactOutcome =
			MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
				AssetData,
				nullptr,
				{},
				*CompanionIndexer,
				ArtifactCache.Get(),
				&DB,
				CompanionArtifactOptions,
				CompanionArtifactResult);

		if (CompanionArtifactOutcome == MonolithAssetArtifactPipeline::EExecuteAssetOutcome::NeedsLocalBuild)
		{
			// companion 和主索引必须一起 promote，所以这里不能静默跳过，
			// 而是同样转交离线 warmup，并让当前 revision 整体放弃提交。
			return QueueOfflineWarmupRequest(
				AssetData,
				*CompanionIndexer,
				TEXT("artifact_cache_miss_requires_warmup"));
		}

		if (CompanionArtifactOutcome != MonolithAssetArtifactPipeline::EExecuteAssetOutcome::Succeeded
			|| !CompanionArtifactResult.bMaterializedProduction)
		{
			return false;
		}

		if (CompanionArtifactResult.bMaterializedShadow && OutShadowDiffIndexers)
		{
			OutShadowDiffIndexers->AddUnique(CompanionIndexer);
		}

		return true;
	};

	TArray<FName> CompanionIndexerIds;
	CompanionIndexersById.GetKeys(CompanionIndexerIds);
	CompanionIndexerIds.Sort([](const FName A, const FName B)
	{
		return A.LexicalLess(B);
	});
	for (const FName CompanionIndexerId : CompanionIndexerIds)
	{
		if (!RunCompanionIndexer(FindIndexerById(CompanionIndexerId)))
		{
			return false;
		}
	}

	return true;
}

bool UMonolithIndexSubsystem::QueueOfflineWarmupRequest(
	const FAssetData& AssetData,
	const IMonolithIndexer& Indexer,
	const TCHAR* Reason) const
{
	FMonolithOfflineWarmupRequest Request;
	Request.PackagePath = AssetData.PackageName.ToString();
	Request.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
	Request.IndexerId = Indexer.GetIndexerId().ToString();
	Request.Reason = Reason ? FString(Reason) : FString(TEXT("gt_quarantine"));
	Request.EnqueuedAtUtc = FDateTime::UtcNow().ToIso8601();

	const bool bQueued = EnqueueMonolithOfflineWarmupRequest(Request);
	if (bQueued)
	{
		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("Queued offline warmup for %s (%s, reason=%s)"),
			*Request.PackagePath,
			*Request.IndexerId,
			*Request.Reason);
	}
	return bQueued;
}

void UMonolithIndexSubsystem::RecordGtWorkSample(const IMonolithIndexer& Indexer, const double DurationSeconds)
{
	GtBudgetState.RecordSample(Indexer.GetIndexerId(), DurationSeconds, FPlatformTime::Seconds());
}

bool UMonolithIndexSubsystem::IsIndexedAssetStaleByMetadata(const FIndexedAsset& Asset) const
{
	// 单次便利重载：load queue 一次 + 走 metadata 单查询。
	// 用在 GetAssetDetails 这种"一次一个"的零星查询。
	TSet<FString> QueuedSet;
	AppendMonolithOfflineWarmupQueuedPackages(QueuedSet);
	return IsIndexedAssetStaleByMetadata(Asset, QueuedSet, /*PreloadedMetadata=*/nullptr);
}

bool UMonolithIndexSubsystem::IsIndexedAssetStaleByMetadata(
	const FIndexedAsset& Asset,
	const TSet<FString>& PreloadedQueuedSet,
	const TMap<int64, FMonolithAssetIndexMetadata>* PreloadedMetadata) const
{
	if (RuntimeState.IsValid() && RuntimeState->IsPackageStale(Asset.PackagePath))
	{
		return true;
	}

	// 用预加载的 queue set 做 O(1) 哈希命中，避免循环里反复 load + parse JSON 文件。
	if (PreloadedQueuedSet.Contains(Asset.PackagePath))
	{
		return true;
	}

	if (!Database.IsValid() || !Database->IsOpen() || Asset.Id <= 0)
	{
		return false;
	}

	const TSharedPtr<IMonolithIndexer>* Indexer = ClassToIndexer.Find(Asset.AssetClass);
	if (!Indexer || !Indexer->IsValid())
	{
		return false;
	}

	// metadata 查询：批量场景走预加载 TMap O(1) 命中；单查场景退回到单条 SQLite prepared statement。
	const FMonolithAssetIndexMetadata* MetadataPtr = nullptr;
	TOptional<FMonolithAssetIndexMetadata> SingleQueryMetadata;
	if (PreloadedMetadata)
	{
		MetadataPtr = PreloadedMetadata->Find(Asset.Id);
	}
	else
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		SingleQueryMetadata = Database->GetAssetIndexMetadataByAssetId(Asset.Id);
		if (SingleQueryMetadata.IsSet())
		{
			MetadataPtr = &SingleQueryMetadata.GetValue();
		}
	}
	if (!MetadataPtr)
	{
		return true;
	}

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const FString ConfiguredProvider = Settings ? Settings->IndexIdentityProvider : FString(TEXT("SavedHash"));
	return MetadataPtr->IndexerId != (*Indexer)->GetIndexerId().ToString()
		|| MetadataPtr->IndexerVersion != (*Indexer)->GetIndexerVersion()
		|| MetadataPtr->ArtifactSchemaVersion != (*Indexer)->GetArtifactSchemaVersion()
		|| !MetadataPtr->IdentityProvider.Equals(ConfiguredProvider, ESearchCase::IgnoreCase);
}

bool UMonolithIndexSubsystem::IsPackageStaleByMetadata(const FString& PackagePath, const FString* AssetClassOverride) const
{
	if (RuntimeState.IsValid() && RuntimeState->IsPackageStale(PackagePath))
	{
		return true;
	}
	if (IsPackageQueuedForMonolithOfflineWarmup(PackagePath))
	{
		return true;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		return false;
	}

	TOptional<FIndexedAsset> Asset;
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		Asset = Database->GetAssetByPath(PackagePath);
	}
	if (!Asset.IsSet())
	{
		return false;
	}

	if (!AssetClassOverride || AssetClassOverride->Equals(Asset->AssetClass, ESearchCase::CaseSensitive))
	{
		return IsIndexedAssetStaleByMetadata(Asset.GetValue());
	}

	FIndexedAsset EffectiveAsset = Asset.GetValue();
	EffectiveAsset.AssetClass = *AssetClassOverride;
	return IsIndexedAssetStaleByMetadata(EffectiveAsset);
}

TSet<FString> UMonolithIndexSubsystem::GatherKnownStalePackages() const
{
	TSet<FString> Result;
	if (RuntimeState.IsValid())
	{
		RuntimeState->AppendStalePackages(Result);
	}

	// 关键性能修复：load 一次 offline warmup queue 进 set，
	// 之后下面 33K+ 资产循环每个 stale 检查走 O(1) 哈希命中。
	// 上一版每次 IsIndexedAssetStaleByMetadata 都重新 load + parse JSON，
	// 在 Monolith.StartIndex 把 AssetVisual companion 全 enqueue 后变成 O(N²) 卡死编辑器。
	TSet<FString> OfflineQueuedPackages;
	AppendMonolithOfflineWarmupQueuedPackages(OfflineQueuedPackages);
	Result.Append(OfflineQueuedPackages);

	if (!Database.IsValid() || !Database->IsOpen())
	{
		return Result;
	}

	// 关键性能修复：把 asset_index_metadata 整张表用单条 SQL 一次性 load 进 TMap，
	// 之后下面 33K+ 资产循环每个 stale check 走 TMap O(1) 命中，
	// 而不是 N 次单独 SQLite prepared statement（之前 6-8s GT 阻塞的根因）。
	TArray<FIndexedAsset> AllAssets;
	TMap<int64, FMonolithAssetIndexMetadata> AllMetadata;
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		AllAssets = Database->GetAllAssets();
		AllMetadata = Database->GetAllAssetIndexMetadata();
	}

	for (const FIndexedAsset& Asset : AllAssets)
	{
		if (IsIndexedAssetStaleByMetadata(Asset, OfflineQueuedPackages, &AllMetadata))
		{
			Result.Add(Asset.PackagePath);
		}
	}

	return Result;
}

void UMonolithIndexSubsystem::AppendRuntimeStats(const TSharedPtr<FJsonObject>& Stats) const
{
	if (!Stats.IsValid())
	{
		return;
	}

	const FMonolithArtifactCacheStats CacheStats = ArtifactCache.IsValid()
		? ArtifactCache->GetStats()
		: FMonolithArtifactCacheStats();
	const FMonolithIndexGtBudgetSnapshot GtSnapshot = GtBudgetState.Snapshot(FPlatformTime::Seconds());
	TArray<FMonolithOfflineWarmupRequest> OfflineQueueRequests;
	LoadMonolithOfflineWarmupQueue(OfflineQueueRequests);

	Stats->SetNumberField(TEXT("local_hit"), static_cast<double>(CacheStats.LocalHitCount));
	Stats->SetNumberField(TEXT("remote_hit"), static_cast<double>(CacheStats.RemoteHitCount));
	Stats->SetNumberField(TEXT("remote_miss"), static_cast<double>(CacheStats.RemoteMissCount));
	Stats->SetNumberField(
		TEXT("cache_read_total"),
		static_cast<double>(CacheStats.LocalHitCount + CacheStats.RemoteHitCount + CacheStats.RemoteMissCount));
	Stats->SetNumberField(
		TEXT("cache_hit_total"),
		static_cast<double>(CacheStats.LocalHitCount + CacheStats.RemoteHitCount));
	Stats->SetNumberField(TEXT("remote_write_ok"), static_cast<double>(CacheStats.RemoteWriteOkCount));
	Stats->SetNumberField(TEXT("remote_write_fail"), static_cast<double>(CacheStats.RemoteWriteFailCount));
	Stats->SetNumberField(TEXT("remote_write_bytes"), static_cast<double>(CacheStats.RemoteWriteBytes));
	Stats->SetNumberField(TEXT("remote_write_mb"), static_cast<double>(CacheStats.RemoteWriteBytes) / (1024.0 * 1024.0));
	Stats->SetNumberField(TEXT("oversized_artifact"), static_cast<double>(CacheStats.OversizedArtifactCount));
	Stats->SetNumberField(TEXT("remote_write_pending"), static_cast<double>(CacheStats.PendingRemoteWriteCount));
	Stats->SetNumberField(TEXT("remote_write_inflight"), static_cast<double>(CacheStats.InFlightRemoteWriteCount));
	Stats->SetNumberField(TEXT("offline_queue_depth"), static_cast<double>(OfflineQueueRequests.Num()));
	Stats->SetBoolField(TEXT("local_cache_available"), ArtifactCache.IsValid());
	Stats->SetStringField(TEXT("local_cache_state"), ArtifactCache.IsValid() ? TEXT("ready") : TEXT("unavailable"));
	Stats->SetBoolField(TEXT("remote_disabled"), CacheStats.bRemoteDisabled);
	Stats->SetStringField(TEXT("remote_cache_state"), CacheStats.bRemoteDisabled ? TEXT("open") : TEXT("ready"));
	Stats->SetNumberField(TEXT("remote_breaker_remaining_seconds"), CacheStats.RemoteBreakerRemainingSeconds);
	Stats->SetNumberField(TEXT("gt_overrun_count"), static_cast<double>(GtSnapshot.OverrunCount));
	Stats->SetNumberField(TEXT("gt_downgrade_count"), static_cast<double>(GtSnapshot.DowngradeCount));
	Stats->SetBoolField(TEXT("gt_breaker_open"), GtSnapshot.bBreakerOpen);
	Stats->SetNumberField(TEXT("gt_breaker_remaining_seconds"), GtSnapshot.BreakerRemainingSeconds);

	// AssetVisual cohort 计数：spec 要求两个 cohort 各暴露一组 local_hit / remote_hit / remote_miss / oversized_artifact。
	// 当前 ArtifactCache 是 cohort-agnostic 单实例（默认 bucket）；AssetVisual cohort 按 spec
	// 走独立 bucket 的 cache 实例，由 reducer 持有。这里把已经有的 row count 作为 cohort 状态摘要暴露，
	// 让上层 UI / monitoring 能立刻看到 AssetVisual 是否有数据。
	if (Database.IsValid() && Database->IsOpen())
	{
		FScopeLock Lock(&DatabaseAccessMutex);
		const TArray<FIndexedAssetVisualEntry> GeoRows = Database->GetAssetVisualEntries(TEXT("AssetVisualGeometric"), FString());
		const TArray<FIndexedAssetVisualEntry> SemRows = Database->GetAssetVisualEntries(TEXT("AssetVisualSemantic"), FString());
		Stats->SetNumberField(TEXT("asset_visual_geometric_row_count"), static_cast<double>(GeoRows.Num()));
		Stats->SetNumberField(TEXT("asset_visual_semantic_row_count"), static_cast<double>(SemRows.Num()));
	}
}

bool UMonolithIndexSubsystem::CanDoIncrementalIndex() const
{
	if (!Database || !Database->IsOpen()) return false;
	FString SchemaVersion = Database->ReadMeta(TEXT("schema_version"));
	if (SchemaVersion.IsEmpty() || FCString::Atoi(*SchemaVersion) < 2)
		return false;
	FString LastFullIndex = Database->ReadMeta(TEXT("last_full_index"));
	if (!LastFullIndex.IsEmpty())
		return true;
	if (Database->HasIndexedAssetSnapshot())
	{
		UE_LOG(
			LogMonolithIndex,
			Warning,
			TEXT("CanDoIncrementalIndex: last_full_index is missing, but an indexed asset snapshot already exists; allowing incremental catch-up"));
		return true;
	}
	return false;
}

void UMonolithIndexSubsystem::StartIncrementalIndex()
{
	check(IsInGameThread());
	if (!IsIndexingEnabled() || bIsIndexing || !Database || !Database->IsOpen())
	{
		return;
	}

	bIsIndexing = true;
	GtBudgetState.Reset();
	UnregisterLiveCallbacks();
	IndexedPlugins = GatherMarketplacePluginPaths();
	IndexingStatusMessage = TEXT("Running incremental index...");

	if (!Scheduler.IsValid())
	{
		Scheduler = MonolithIndexInternal::MakeScheduler();
		if (ArtifactCache.IsValid())
		{
			ArtifactCache->SetIoThreadPool(Scheduler->GetIoDdcThreadPool());
		}
	}

	const TArray<FString> ValidPrefixes = BuildIndexedPackagePrefixes(IndexedPlugins);
	TSet<FName> CurrentPackages;
	TMap<FName, FIoHash> CurrentHashes;
	TMap<FName, TArray<FAssetData>> CurrentAssetsByPackage;
	{
		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		MonolithIndexInternal::CollectManagedAssetSnapshotFromRegistry(
			AssetRegistry,
			ValidPrefixes,
			nullptr,
			&CurrentPackages,
			&CurrentHashes,
			&CurrentAssetsByPackage);
	}

	const bool bStarted = Scheduler.IsValid() && Scheduler->StartBackgroundJob(
		[this,
		 CurrentPackages = MoveTemp(CurrentPackages),
		 CurrentHashes = MoveTemp(CurrentHashes),
		 CurrentAssetsByPackage = MoveTemp(CurrentAssetsByPackage)]() mutable
		{
			RunIncrementalIndexJob(
				MoveTemp(CurrentPackages),
				MoveTemp(CurrentHashes),
				MoveTemp(CurrentAssetsByPackage));
		},
		EQueuedWorkPriority::Low);
	if (!bStarted)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to schedule incremental Monolith index job"));
		bIsIndexing = false;
		IndexingStatusMessage.Empty();
		RegisterLiveCallbacks();
		return;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Incremental index scheduled on Monolith scheduler"));
}

void UMonolithIndexSubsystem::RunIncrementalIndexJob(
	TSet<FName> InCurrentPackages,
	TMap<FName, FIoHash> InCurrentHashes,
	TMap<FName, TArray<FAssetData>> InAssetsByPackage)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		FinalizeLiveDatabaseJob(false, true, TEXT("Incremental index complete."), TEXT("Incremental index failed because the database is unavailable."));
		return;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting incremental index..."));

	TSet<FName> CurrentPackages = MoveTemp(InCurrentPackages);
	TMap<FName, FIoHash> CurrentHashes = MoveTemp(InCurrentHashes);
	TMap<FName, TArray<FAssetData>> CurrentAssetsByPackage = MoveTemp(InAssetsByPackage);

	bool bSuccess = false;
	{
		FScopeLock DatabaseLock(&DatabaseAccessMutex);

		TMap<FString, FString> DBPathsAndHashes = Database->GetAllPathsAndHashes();
		TSet<FName> DBPackages;
		TMap<FName, FIoHash> DBHashes;
		for (const auto& [Path, Hash] : DBPathsAndHashes)
		{
			const FName PathName(*Path);
			DBPackages.Add(PathName);
			if (!Hash.IsEmpty())
			{
				FIoHash IoHash;
				LexFromString(IoHash, *Hash);
				DBHashes.Add(PathName, IoHash);
			}
		}

		TArray<FName> AddedPaths;
		TArray<FName> DeletedPaths;
		TArray<FName> ExistingPaths;
		for (FName Pkg : CurrentPackages)
		{
			if (!DBPackages.Contains(Pkg))
			{
				AddedPaths.Add(Pkg);
			}
			else
			{
				ExistingPaths.Add(Pkg);
			}
		}
		for (FName Pkg : DBPackages)
		{
			if (!CurrentPackages.Contains(Pkg))
			{
				DeletedPaths.Add(Pkg);
			}
		}

		TMultiMap<FIoHash, FName> DeletedHashMap;
		for (FName Deleted : DeletedPaths)
		{
			if (FIoHash* Hash = DBHashes.Find(Deleted))
			{
				if (!Hash->IsZero())
				{
					DeletedHashMap.Add(*Hash, Deleted);
				}
			}
		}

		TArray<TPair<FName, FName>> Moves;
		TArray<FName> TrueAdds;
		for (FName Added : AddedPaths)
		{
			FIoHash* NewHash = CurrentHashes.Find(Added);
			if (NewHash && !NewHash->IsZero())
			{
				TArray<FName> FoundOldPaths;
				DeletedHashMap.MultiFind(*NewHash, FoundOldPaths);
				if (FoundOldPaths.Num() > 0)
				{
					const FName MatchedOldPath = FoundOldPaths[0];
					DeletedHashMap.RemoveSingle(*NewHash, MatchedOldPath);
					Moves.Add({MatchedOldPath, Added});
					continue;
				}
			}
			TrueAdds.Add(Added);
		}

		TSet<FName> MovedOldPaths;
		for (const auto& [OldPath, NewPath] : Moves)
		{
			MovedOldPaths.Add(OldPath);
		}

		TArray<FName> TrueDeletes;
		for (FName Deleted : DeletedPaths)
		{
			if (!MovedOldPaths.Contains(Deleted))
			{
				TrueDeletes.Add(Deleted);
			}
		}

		TArray<FName> ModifiedPaths;
		for (FName Existing : ExistingPaths)
		{
			FIoHash* CurrentHash = CurrentHashes.Find(Existing);
			FIoHash* StoredHash = DBHashes.Find(Existing);
			if ((CurrentHash && StoredHash && *CurrentHash != *StoredHash)
				|| (CurrentHash && !StoredHash))
			{
				ModifiedPaths.Add(Existing);
			}
		}

		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("Incremental delta: %d added, %d deleted, %d moved, %d modified, %d unchanged"),
			TrueAdds.Num(),
			TrueDeletes.Num(),
			Moves.Num(),
			ModifiedPaths.Num(),
			ExistingPaths.Num() - ModifiedPaths.Num());

		if (TrueDeletes.Num() == 0 && TrueAdds.Num() == 0 && Moves.Num() == 0 && ModifiedPaths.Num() == 0)
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("No changes detected. Incremental index complete."));
			bSuccess = true;
		}
		else
		{
			Database->BeginTransaction();

			for (FName Path : TrueDeletes)
			{
				Database->DeleteAssetByPath(Path.ToString());
			}

			for (const auto& [OldPath, NewPath] : Moves)
			{
				Database->UpdateAssetPath(OldPath.ToString(), NewPath.ToString());
				if (FIoHash* Hash = CurrentHashes.Find(NewPath))
				{
					Database->UpdateSavedHash(NewPath.ToString(), LexToString(*Hash));
				}
			}

			TSet<FName> PathsToIndex;
			TSet<FString> CompanionOnlyPaths;
			for (FName Path : TrueAdds)
			{
				PathsToIndex.Add(Path);
			}
			for (FName Path : ModifiedPaths)
			{
				PathsToIndex.Add(Path);
			}
			for (const auto& [OldPath, NewPath] : Moves)
			{
				FIoHash* CurrentHash = CurrentHashes.Find(NewPath);
				FIoHash* StoredHash = DBHashes.Find(OldPath);
				if (CurrentHash && StoredHash && *CurrentHash != *StoredHash)
				{
					PathsToIndex.Add(NewPath);
				}
			}

			for (FName Path : PathsToIndex)
			{
				const FString PathStr = Path.ToString();
				const int64 AssetId = Database->GetAssetId(PathStr);

				const TArray<FAssetData>* Assets = CurrentAssetsByPackage.Find(Path);
				if (!Assets || Assets->Num() == 0)
				{
					continue;
				}

				const FAssetData& AssetData = (*Assets)[0];
				FIndexedAsset IndexedAsset = MonolithIndexInternal::BuildIndexedAssetRecord(
					AssetData,
					IndexedPlugins,
					&CurrentHashes);

				if (AssetId > 0)
				{
					Database->UpdateAssetMetadata(IndexedAsset);
				}
				else
				{
					Database->InsertAsset(IndexedAsset);
				}

				if (!ClassToIndexer.Contains(IndexedAsset.AssetClass))
				{
					CompanionOnlyPaths.Add(PathStr);
				}
			}

			const TSet<FString> PathStrings = MonolithIndexInternal::CollectPackagePaths(PathsToIndex);
			TSet<FString> StalePackages = PathStrings;
			MonolithIndexInternal::AddPackagePaths(StalePackages, TrueDeletes);
			MonolithIndexInternal::AddPackagePaths(StalePackages, Moves);
			if (RuntimeState.IsValid())
			{
				RuntimeState->BeginSession(StalePackages, PathStrings.Num());
			}
			ProcessDeepIndexQueue(CurrentAssetsByPackage, PathStrings, EMonolithArtifactCacheRequestMode::Background);
			Database->CommitTransaction();

			ProcessCompanionOnlyPackages(
				CurrentAssetsByPackage,
				CompanionOnlyPaths,
				EMonolithArtifactCacheRequestMode::Background,
				*Database);

			UE_LOG(LogMonolithIndex, Log, TEXT("Incremental index complete."));
			bSuccess = true;
		}
	}

	FinalizeLiveDatabaseJob(
		bSuccess,
		true,
		TEXT("Incremental index complete."),
		TEXT("Incremental index failed."));
}

void UMonolithIndexSubsystem::FinalizeLiveDatabaseJob(
	const bool bSuccess,
	const bool bReRegisterLiveCallbacks,
	const TCHAR* SuccessMessage,
	const TCHAR* FailureMessage)
{
	const FString SuccessText = SuccessMessage ? FString(SuccessMessage) : FString();
	const FString FailureText = FailureMessage ? FString(FailureMessage) : FString(TEXT("Monolith background index job failed."));
	AsyncTask(ENamedThreads::GameThread, [this, bSuccess, bReRegisterLiveCallbacks, SuccessText, FailureText]()
	{
		if (RuntimeState.IsValid())
		{
			RuntimeState->FinishSession();
		}

		bIsIndexing = false;
		if (bSuccess)
		{
			IndexingStatusMessage.Empty();
			if (!SuccessText.IsEmpty())
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("%s"), *SuccessText);
			}
		}
		else
		{
			IndexingStatusMessage = FailureText;
			UE_LOG(LogMonolithIndex, Error, TEXT("%s"), *FailureText);
		}

		if (bReRegisterLiveCallbacks && IsIndexingEnabled() && Database.IsValid() && Database->IsOpen())
		{
			RegisterLiveCallbacks();
		}
	});
}

// ============================================================
// Stubs for Tasks 5-6
// ============================================================

void UMonolithIndexSubsystem::ProcessDeepIndexQueue(
	const TMap<FName, TArray<FAssetData>>& AssetsByPackage,
	const TSet<FString>& PathsToIndex,
	const EMonolithArtifactCacheRequestMode RequestMode)
{
	if (PathsToIndex.Num() == 0)
	{
		return;
	}

	int32 Indexed = 0;
	int32 ProcessedPaths = 0;

	for (const FString& PackagePath : PathsToIndex)
	{
		const TArray<FAssetData>* Assets = AssetsByPackage.Find(FName(*PackagePath));
		if (!Assets)
		{
			++ProcessedPaths;
			if (RuntimeState.IsValid())
			{
				RuntimeState->UpdateProgress(ProcessedPaths, PathsToIndex.Num());
			}
			continue;
		}

		for (const FAssetData& AssetData : *Assets)
		{
			const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
			TSharedPtr<IMonolithIndexer>* Indexer = ClassToIndexer.Find(ClassName);
			if (!Indexer)
			{
				continue;
			}

			const int64 AssetId = Database->GetAssetId(PackagePath);
			if (AssetId <= 0)
			{
				continue;
			}

			bool bDeferredToOfflineWarmup = false;
			if (!TryIndexPrimaryAssetInRevision(
				AssetData,
				*Indexer,
				AssetId,
				RequestMode,
				*Database,
				bDeferredToOfflineWarmup))
			{
				continue;
			}

			++Indexed;
		}

		++ProcessedPaths;
		if (RuntimeState.IsValid())
		{
			RuntimeState->UpdateProgress(ProcessedPaths, PathsToIndex.Num());
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep-indexed %d assets from %d paths"), Indexed, PathsToIndex.Num());
}

void UMonolithIndexSubsystem::ProcessCompanionOnlyPackages(
	const TMap<FName, TArray<FAssetData>>& AssetsByPackage,
	const TSet<FString>& PackagePaths,
	const EMonolithArtifactCacheRequestMode RequestMode,
	FMonolithIndexDatabase& DB,
	const TAtomic<bool>* StopFlag)
{
	// 这条 helper 现在承担“没有主 deep indexer 的包，也要补写 companion 数据”这件事。
	//
	// 以前这里靠 scoped sentinel 做兜底：
	// - Dependency / GameplayTags 还要保留 fake supported class；
	// - 子系统还要额外区分一批 sentinel indexer；
	// - full / incremental / live 三条链也会各自绕出一条旁路。
	//
	// 现在我们改成唯一实现：
	// 1. 收到一批 companion-only 包路径；
	// 2. 直接消费调用方在 GT 上抓好的真实 AssetData 快照；
	// 3. 按稳定顺序分批跑 `TryIndexCompanionOnlyAssetInRevision`；
	// 4. 每个包都走同一套 revision / promote / shadow diff 规则。
	if (PackagePaths.Num() == 0)
	{
		return;
	}

	TArray<FString> SortedPackagePaths = PackagePaths.Array();
	SortedPackagePaths.Sort();

	struct FCompanionOnlyWorkItem
	{
		/** 真实资产的 AssetData。 */
		FAssetData AssetData;
		/** 这份资产在当前数据库里的主键。 */
		int64 AssetId = 0;
	};

	TArray<FCompanionOnlyWorkItem> WorkItems;
	WorkItems.Reserve(SortedPackagePaths.Num());

	for (const FString& PackagePath : SortedPackagePaths)
	{
		if (StopFlag && StopFlag->Load())
		{
			break;
		}

		const int64 AssetId = DB.GetAssetId(PackagePath);
		if (AssetId <= 0)
		{
			continue;
		}

		const TArray<FAssetData>* Assets = AssetsByPackage.Find(FName(*PackagePath));
		if (!Assets || Assets->Num() == 0)
		{
			continue;
		}

		// 这里本来只应该收到“没有主 indexer”的包。
		// 但 helper 还是再做一次兜底判断，避免未来调用方误把 deep-index 资产塞进来。
		const FString AssetClass = (*Assets)[0].AssetClassPath.GetAssetName().ToString();
		if (ClassToIndexer.Contains(AssetClass))
		{
			continue;
		}

		FCompanionOnlyWorkItem& WorkItem = WorkItems.AddDefaulted_GetRef();
		WorkItem.AssetData = (*Assets)[0];
		WorkItem.AssetId = AssetId;
	}

	if (WorkItems.Num() == 0)
	{
		return;
	}

	IndexingStatusMessage = FString::Printf(TEXT("Indexing companion data for %d assets..."), WorkItems.Num());
	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing companion data for %d companion-only assets..."), WorkItems.Num());

	const int32 CompanionBatchSize = 256;
	for (int32 BatchStart = 0; BatchStart < WorkItems.Num(); BatchStart += CompanionBatchSize)
	{
		if (StopFlag && StopFlag->Load())
		{
			break;
		}

		const int32 BatchEnd = FMath::Min(BatchStart + CompanionBatchSize, WorkItems.Num());
		DB.BeginTransaction();
		for (int32 ItemIndex = BatchStart; ItemIndex < BatchEnd; ++ItemIndex)
		{
			if (StopFlag && StopFlag->Load())
			{
				break;
			}

			const FCompanionOnlyWorkItem& WorkItem = WorkItems[ItemIndex];
			if (!TryIndexCompanionOnlyAssetInRevision(
				WorkItem.AssetData,
				WorkItem.AssetId,
				RequestMode,
				DB))
			{
				UE_LOG(
					LogMonolithIndex,
					Warning,
					TEXT("Companion indexers failed for %s"),
					*WorkItem.AssetData.PackageName.ToString());
			}
		}
		DB.CommitTransaction();
	}
}

void UMonolithIndexSubsystem::RegisterLiveCallbacks()
{
	IAssetRegistry& AR = IAssetRegistry::GetChecked();

	OnAssetsAddedHandle = AR.OnAssetsAdded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsAddedCallback);
	OnAssetsRemovedHandle = AR.OnAssetsRemoved().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsRemovedCallback);
	OnAssetRenamedHandle = AR.OnAssetRenamed().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRenamedCallback);
	OnAssetsUpdatedOnDiskHandle = AR.OnAssetsUpdatedOnDisk().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback);

	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			LiveIndexTimerHandle,
			FTimerDelegate::CreateUObject(this, &UMonolithIndexSubsystem::ProcessPendingChanges),
			2.0f, /*bLoop=*/ true);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Live index callbacks registered."));
}

void UMonolithIndexSubsystem::UnregisterLiveCallbacks()
{
	if (IAssetRegistry* AR = IAssetRegistry::Get())
	{
		AR->OnAssetsAdded().Remove(OnAssetsAddedHandle);
		AR->OnAssetsRemoved().Remove(OnAssetsRemovedHandle);
		AR->OnAssetRenamed().Remove(OnAssetRenamedHandle);
		AR->OnAssetsUpdatedOnDisk().Remove(OnAssetsUpdatedOnDiskHandle);
	}
	OnAssetsAddedHandle.Reset();
	OnAssetsRemovedHandle.Reset();
	OnAssetRenamedHandle.Reset();
	OnAssetsUpdatedOnDiskHandle.Reset();

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(LiveIndexTimerHandle);
	}
}

// ============================================================
// Live AR callback handlers
// ============================================================

static bool IsRedirector(const FAssetData& AssetData)
{
	static const FTopLevelAssetPath RedirectorPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector"));
	return AssetData.AssetClassPath == RedirectorPath;
}

void UMonolithIndexSubsystem::OnAssetsAddedCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData) && IsPackagePathIndexed(AssetData.PackageName.ToString()))
			PendingChanges.Add({EIndexChangeType::Added, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetsRemovedCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData) && IsPackagePathIndexed(AssetData.PackageName.ToString()))
			PendingChanges.Add({EIndexChangeType::Removed, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetRenamedCallback(const FAssetData& AssetData, const FString& OldObjectPath)
{
	if (bIsIndexing) return;
	FString OldPackagePath;
	FString IgnoredAssetName;
	OldObjectPath.Split(TEXT("."), &OldPackagePath, &IgnoredAssetName);
	if (!IsPackagePathIndexed(AssetData.PackageName.ToString()) && !IsPackagePathIndexed(OldPackagePath))
	{
		return;
	}
	PendingChanges.Add({EIndexChangeType::Renamed, AssetData, OldObjectPath});
}

void UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
	{
		if (IsPackagePathIndexed(AssetData.PackageName.ToString()))
		{
			PendingChanges.Add({EIndexChangeType::Updated, AssetData, {}});
		}
	}
}

void UMonolithIndexSubsystem::ProcessPendingChanges()
{
	if (PendingChanges.Num() == 0 || !IsIndexingEnabled() || !Database || !Database->IsOpen() || bIsIndexing)
	{
		return;
	}

	bIsIndexing = true;
	GtBudgetState.Reset();
	IndexingStatusMessage = TEXT("Processing live index updates...");
	UnregisterLiveCallbacks();

	TArray<FPendingIndexChange> RawChanges = MoveTemp(PendingChanges);
	TArray<FPendingIndexChange> RetryChanges = RawChanges;
	PendingChanges.Reset();

	if (!Scheduler.IsValid())
	{
		Scheduler = MonolithIndexInternal::MakeScheduler();
		if (ArtifactCache.IsValid())
		{
			ArtifactCache->SetIoThreadPool(Scheduler->GetIoDdcThreadPool());
		}
	}

	TSet<FName> ChangedPackages;
	for (const FPendingIndexChange& Change : RawChanges)
	{
		switch (Change.Type)
		{
		case EIndexChangeType::Added:
		case EIndexChangeType::Updated:
		case EIndexChangeType::Renamed:
			ChangedPackages.Add(Change.AssetData.PackageName);
			break;
		case EIndexChangeType::Removed:
			break;
		}
	}

	TMap<FName, TArray<FAssetData>> AssetsByPackage;
	TMap<FName, FIoHash> CurrentHashes;
	if (ChangedPackages.Num() > 0)
	{
		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		MonolithIndexInternal::CollectPackageAssetSnapshotFromRegistry(
			AssetRegistry,
			ChangedPackages,
			AssetsByPackage,
			&CurrentHashes);
	}

	const bool bStarted = Scheduler.IsValid() && Scheduler->StartBackgroundJob(
		[this,
		 LocalChanges = MoveTemp(RawChanges),
		 AssetsByPackage = MoveTemp(AssetsByPackage),
		 CurrentHashes = MoveTemp(CurrentHashes)]() mutable
		{
			RunLiveUpdateJob(
				MoveTemp(LocalChanges),
				MoveTemp(AssetsByPackage),
				MoveTemp(CurrentHashes));
		},
		EQueuedWorkPriority::Low);
	if (!bStarted)
	{
		bIsIndexing = false;
		IndexingStatusMessage.Empty();
		PendingChanges = MoveTemp(RetryChanges);
		RegisterLiveCallbacks();
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to schedule live Monolith index update job"));
	}
}

void UMonolithIndexSubsystem::RunLiveUpdateJob(
	TArray<FPendingIndexChange> InRawChanges,
	TMap<FName, TArray<FAssetData>> InAssetsByPackage,
	TMap<FName, FIoHash> InCurrentHashes)
{
	if (!Database.IsValid() || !Database->IsOpen())
	{
		FinalizeLiveDatabaseJob(false, true, TEXT("Live index update complete."), TEXT("Live index update failed because the database is unavailable."));
		return;
	}

	TArray<FPendingIndexChange> RawChanges = MoveTemp(InRawChanges);
	TMap<FName, TArray<FAssetData>> AssetsByPackage = MoveTemp(InAssetsByPackage);
	TMap<FName, FIoHash> CurrentHashes = MoveTemp(InCurrentHashes);

	TMap<FName, int32> PathToLastIndex;
	TArray<FPendingIndexChange> LocalChanges;
	LocalChanges.Reserve(RawChanges.Num());

	for (int32 i = 0; i < RawChanges.Num(); ++i)
	{
		const FName PkgName = RawChanges[i].AssetData.PackageName;
		if (int32* ExistingIdx = PathToLastIndex.Find(PkgName))
		{
			const EIndexChangeType PrevType = LocalChanges[*ExistingIdx].Type;
			const EIndexChangeType NewType = RawChanges[i].Type;

			if (PrevType == EIndexChangeType::Renamed && NewType == EIndexChangeType::Updated)
			{
				continue;
			}
			if (PrevType == EIndexChangeType::Removed && NewType == EIndexChangeType::Added)
			{
				RawChanges[i].Type = EIndexChangeType::Updated;
			}

			LocalChanges[*ExistingIdx] = MoveTemp(RawChanges[i]);
		}
		else
		{
			PathToLastIndex.Add(PkgName, LocalChanges.Num());
			LocalChanges.Add(MoveTemp(RawChanges[i]));
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Processing %d pending index changes (%d raw)"),
		LocalChanges.Num(), RawChanges.Num());

	{
		FScopeLock DatabaseLock(&DatabaseAccessMutex);
		Database->BeginTransaction();

		TSet<FString> PathsToDeepIndex;
		TSet<FString> CompanionOnlyPaths;
		TSet<FString> StalePackages;

		for (const FPendingIndexChange& Change : LocalChanges)
		{
			switch (Change.Type)
			{
			case EIndexChangeType::Added:
			{
				FIndexedAsset IndexedAsset = MonolithIndexInternal::BuildIndexedAssetRecord(
					Change.AssetData,
					IndexedPlugins,
					&CurrentHashes);

				Database->InsertAsset(IndexedAsset);

				const FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
				StalePackages.Add(IndexedAsset.PackagePath);
				if (ClassToIndexer.Contains(ClassName))
				{
					PathsToDeepIndex.Add(IndexedAsset.PackagePath);
				}
				else
				{
					CompanionOnlyPaths.Add(IndexedAsset.PackagePath);
				}
				break;
			}
			case EIndexChangeType::Updated:
			{
				FIndexedAsset IndexedAsset = MonolithIndexInternal::BuildIndexedAssetRecord(
					Change.AssetData,
					IndexedPlugins,
					&CurrentHashes);

				const int64 AssetId = Database->GetAssetId(IndexedAsset.PackagePath);
				if (AssetId > 0)
				{
					Database->UpdateAssetMetadata(IndexedAsset);
				}
				else
				{
					Database->InsertAsset(IndexedAsset);
				}

				const FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
				StalePackages.Add(IndexedAsset.PackagePath);
				if (ClassToIndexer.Contains(ClassName))
				{
					PathsToDeepIndex.Add(IndexedAsset.PackagePath);
				}
				else
				{
					CompanionOnlyPaths.Add(IndexedAsset.PackagePath);
				}
				break;
			}
			case EIndexChangeType::Removed:
			{
				const FString Path = Change.AssetData.PackageName.ToString();
				Database->DeleteAssetByPath(Path);
				StalePackages.Add(Path);
				break;
			}
			case EIndexChangeType::Renamed:
			{
				FString OldPackageName;
				FString IgnoredOldAssetName;
				Change.OldObjectPath.Split(TEXT("."), &OldPackageName, &IgnoredOldAssetName);
				const FIndexedAsset IndexedAsset = MonolithIndexInternal::BuildIndexedAssetRecord(
					Change.AssetData,
					IndexedPlugins,
					&CurrentHashes);
				const FString& NewPath = IndexedAsset.PackagePath;
				const FString& NewAssetName = IndexedAsset.AssetName;
				StalePackages.Add(OldPackageName);
				StalePackages.Add(NewPath);

				if (Database->UpdateAssetPath(OldPackageName, NewPath, NewAssetName))
				{
					Database->UpdateAssetMetadata(IndexedAsset);
					UE_LOG(LogMonolithIndex, Verbose, TEXT("Asset moved: %s -> %s"), *OldPackageName, *NewPath);
				}
				else
				{
					Database->InsertAsset(IndexedAsset);
					if (ClassToIndexer.Contains(IndexedAsset.AssetClass))
					{
						PathsToDeepIndex.Add(NewPath);
					}
					else
					{
						CompanionOnlyPaths.Add(NewPath);
					}
				}
				break;
			}
			}
		}

		if (PathsToDeepIndex.Num() > 0)
		{
			if (RuntimeState.IsValid())
			{
				RuntimeState->BeginSession(StalePackages, PathsToDeepIndex.Num());
			}
			ProcessDeepIndexQueue(AssetsByPackage, PathsToDeepIndex, EMonolithArtifactCacheRequestMode::Interactive);
		}
		else if (RuntimeState.IsValid() && StalePackages.Num() > 0)
		{
			RuntimeState->BeginSession(StalePackages, 0);
		}

		Database->CommitTransaction();

		if (CompanionOnlyPaths.Num() > 0)
		{
			ProcessCompanionOnlyPackages(
				AssetsByPackage,
				CompanionOnlyPaths,
				EMonolithArtifactCacheRequestMode::Interactive,
				*Database);
		}
	}

	FinalizeLiveDatabaseJob(
		true,
		true,
		TEXT("Live index update complete."),
		TEXT("Live index update failed."));
}
