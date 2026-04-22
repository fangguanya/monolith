#pragma once

#include "CoreMinimal.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexDatabase.h"

/*
 * 这份头文件定义了“索引器”的公共接口。
 *
 * 可以把整个 MonolithIndex 想成一家分工很细的仓库：
 * - 数据库负责把结果存起来；
 * - 调度器负责安排什么时候干活；
 * - 每个 Indexer 负责“看懂一种资产，然后把它翻译成统一的索引数据”。
 *
 * 所以 IMonolithIndexer 就像所有工种都要遵守的一份统一工作表。
 * 新增一种资产支持时，通常就是继承它，然后实现几件事：
 * 1. 我支持哪些资产类；
 * 2. 我怎么把资产内容写进 SQLite；
 * 3. 如果走 artifact/DDC 新链路，我怎么构建和回放 artifact；
 * 4. 如果我是 sentinel（全局后处理），我怎么做 scoped 更新。
 */

class IAssetRegistry;
struct FAssetData;

/*
 * IMonolithIndexer 是所有索引器都要实现的统一接口。
 *
 * 你可以把它想成“工单模板”：
 * - 先声明自己会处理什么资产；
 * - 再定义怎么把资产翻译成数据库能存的结构；
 * - 如果走 artifact 路径，还要会把结果打包和回放；
 * - 如果自己是 sentinel，还要会做 scoped 增量更新。
 */
class MONOLITHINDEX_API IMonolithIndexer
{
public:
	/** 虚析构函数。
	 * 这样外面就算是拿接口指针删除具体子类，也能把子类资源正确释放掉。 */
	virtual ~IMonolithIndexer() = default;

	/** 告诉系统“我认哪些资产类”。
	 * 例如 BlueprintIndexer 会返回 Blueprint，MaterialIndexer 会返回 Material。 */
	virtual TArray<FString> GetSupportedClasses() const = 0;

	/** 判断“这一份具体资产”是否真的应该交给当前 indexer 处理。
	 *
	 * 默认实现只做最朴素的类名匹配：
	 * - 资产类名命中 `GetSupportedClasses()` 里的任意一项，就返回 true；
	 * - 否则返回 false。
	 *
	 * 之所以要把这个入口单独做成接口，是因为有些 indexer 不能只看“资产类短名”：
	 * - `MeshCatalog` 这种 companion 虽然逻辑上只处理 `StaticMesh`，但不参与主 class dispatch；
	 * - `GAS` 这种 Blueprint companion 需要继续往下看 `ParentClass / NativeParentClass` 标签，
	 *   不能因为资产类同样都是 `Blueprint` 就对所有蓝图都盲跑一遍。
	 *
	 * `LoadedAsset` 是可选的“已加载对象”补充信息：
	 * - 大多数 indexer 只看 `AssetData` 就够了；
	 * - 某些 indexer 也可以在对象已经加载时，用它做更精确的二次确认。
	 */
	virtual bool MatchesAsset(const FAssetData& AssetData, const UObject* LoadedAsset = nullptr) const
	{
		(void)LoadedAsset;

		const FString AssetClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		for (const FString& SupportedClass : GetSupportedClasses())
		{
			if (SupportedClass == AssetClassName)
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * 给一个已经加载好的资产做深度索引。
	 *
	 * 现在包级 indexer 的默认实现已经统一收口到 artifact 主链：
	 * 1. 让 indexer 自己先调用 `BuildArtifact(...)` 构建稳定 payload；
	 * 2. 再调用 `MaterializeArtifact(...)` 把同一份 payload 回放到正式表。
	 *
	 * 这样 package-scoped 路径就不会再出现
	 * “一套直接写库逻辑 + 一套 artifact 逻辑”的重复实现。
	 *
	 * 对于 `GlobalReducer` 类型的 indexer，默认实现会直接转发到 `IndexGlobal(...)`，
	 * 因为它们本来就不对应单个真实资产。
	 */
	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId);

	/**
	 * 给“不对应单个资产”的全局 indexer 一个统一入口。
	 *
	 * 典型例子：
	 * - `ConfigIndexer` 会扫一批 `.ini` 文件；
	 * - `CppIndexer` 会扫一批 `.h/.cpp` 文件；
	 * - 某些 reducer / 全局定义刷新逻辑也属于这一类。
	 *
	 * 以前这些逻辑只能靠“塞一个假 AssetData，再调 IndexAsset”来触发，
	 * 这会让调用方很难一眼看懂“它到底是不是资产级索引”。
	 *
	 * 现在统一改成：
	 * - 单资产索引走 `IndexAsset`；
	 * - 全局型索引走 `IndexGlobal`。
	 *
	 * 默认返回 false，表示“我不支持这种运行方式”。
	 */
	virtual bool IndexGlobal(FMonolithIndexDatabase& DB)
	{
		(void)DB;
		return false;
	}

	/** 返回给日志和调试面板看的名字。 */
	virtual FString GetName() const = 0;

	/** 返回稳定的 indexer 身份名。
	 * artifact identity 会把它算进去，用来区分“到底是哪种工人产出的结果”。 */
	virtual FName GetIndexerId() const { return FName(*GetName()); }

	/** 逻辑版本号。
	 * 如果索引算法改了，旧 artifact 可能就不可靠了，这时提高版本号就能让缓存失效重建。 */
	virtual uint32 GetIndexerVersion() const { return 1; }

	/** artifact 载荷格式版本号。
	 * 这个版本关注的是“二进制长什么样”，而不是“业务逻辑怎么想”。 */
	virtual uint8 GetArtifactSchemaVersion() const { return 1; }

	/** 额外依赖版本列表。
	 * 当某个外部格式、辅助函数或共享序列化逻辑变化时，可以把版本盐带进 identity。 */
	virtual TArray<FMonolithDependencyVersion> GetDependencyVersions() const { return {}; }

	/** 这个 indexer 更适合哪种执行模式。
	 * 调度器和 warmup 会根据它决定是按包加载、只读元数据，还是其它方式。 */
	virtual EMonolithExecutionMode GetExecutionMode() const { return EMonolithExecutionMode::PackageScopedLoad; }

	/** 可选：把“索引结果”先打包成 artifact。
	 * 这样下次就可能直接从 DDC 回放，而不是重新加载资产再算一遍。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
	{
		return false;
	}

	/**
	 * 给“全局型 indexer”构建一份稳定 identity。
	 *
	 * 这和包级 identity 的目标一样：
	 * - 输入没变，就复用旧 artifact；
	 * - 输入变了，就重新构建。
	 *
	 * 只是它描述的对象不再是“某一个包”，
	 * 而是一整份全局快照，例如：
	 * - Config 全量扫描结果；
	 * - Cpp 符号全量扫描结果；
	 * - GameplayTag 定义树快照。
	 */
	virtual bool BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const
	{
		(void)OutIdentity;
		return false;
	}

	/**
	 * 为全局型 indexer 构建 artifact。
	 *
	 * 它和 `BuildArtifact` 的区别只有一点：
	 * - `BuildArtifact` 面向单个资产；
	 * - `BuildGlobalArtifact` 面向整份全局结果。
	 */
	virtual bool BuildGlobalArtifact(FMonolithArtifact& OutArtifact)
	{
		(void)OutArtifact;
		return false;
	}

	/** 把已经拿到的 artifact 回放到正式 SQLite 表里。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
	{
		return false;
	}

	/** 把全局 artifact 回放到正式 SQLite 表里。 */
	virtual bool MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB)
	{
		(void)Artifact;
		(void)DB;
		return false;
	}

	/** 把 artifact 回放到 shadow 表里。
	 * shadow 表像一块“预演沙盘”，先比对结果，确认没问题再决定是否 promote。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
	{
		return false;
	}

	/** 是否是 sentinel indexer。
	 * sentinel 不按“一个资产进来处理一个资产”工作，而是自己扫一批全局信息。 */
	virtual bool IsSentinel() const { return false; }

	/** 这个 sentinel 是否支持 scoped 增量更新。
	 * 支持的话，改动少量资产时就不用把全局再重扫一遍。 */
	virtual bool SupportsIncrementalIndex() const { return false; }

	/** 只重建指定变化范围的数据。
	 * 它通常发生在主事务提交之后，所以实现里需要自己决定是否再开事务。 */
	virtual bool IndexScoped(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths, FMonolithIndexDatabase& DB) { return false; }
};
