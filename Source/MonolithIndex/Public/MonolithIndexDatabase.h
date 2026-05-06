#pragma once

#include "CoreMinimal.h"
#include "IO/IoHash.h"
#include "SQLiteDatabase.h"
#include "MonolithIndexLog.h"
#include "AssetVisualEntry.h"

/*
 * 这份头文件描述的是 Monolith 的本地索引数据库长什么样、能做什么。
 *
 * 如果把 SQLite 想成一本“项目内容说明书”，这里面定义的 struct 就是说明书里的表格行。
 * 例如：
 * - FIndexedAsset 表示“一个资产”的基础信息；
 * - FIndexedNode / Variable / Parameter / Actor / DataTableRow 表示更细的子数据；
 * - FMonolithAssetIndexMetadata 记录这行数据是用哪个 indexer、哪个版本算出来的。
 *
 * Phase 4 之后，这里还承担了一个很关键的职责：
 * - 对同一个资产支持 revision 写入；
 * - 在新 revision 完成前，查询仍然能看到旧 revision；
 * - 只有 promote 成功后，current_revision_id 才会切到新版本。
 *
 * 这样做的目的很朴素：
 * “不要因为索引正在更新，就让查询一瞬间什么都看不到。”
 */

struct FIndexedAsset
{
	/** 数据库里的主键 id。 */
	int64 Id = 0;
	/** 资产包路径，例如 /Game/Foo/Bar。 */
	FString PackagePath;
	/** 资产短名，例如 Bar。 */
	FString AssetName;
	/** 资产类型名，例如 Blueprint、Material。 */
	FString AssetClass;
	/** 所属模块或插件名。 */
	FString ModuleName;
	/** 给搜索结果和详情面板显示的描述文字。 */
	FString Description;
	/** 文件大小，单位字节。 */
	int64 FileSizeBytes = 0;
	/** 文件最后修改时间。 */
	FString LastModified;
	/** 当前记录对应的保存哈希。 */
	FString SavedHash;
	/** 最近一次成功索引的时间。 */
	FString IndexedAt;
	/** 当前对外可见的 revision id。 */
	int64 CurrentRevisionId = 0;
};

struct FIndexedNode
{
	/** 节点行自己的主键。 */
	int64 Id = 0;
	/** 它属于哪个资产。 */
	int64 AssetId = 0;
	/** 它属于资产的哪个 revision。 */
	int64 RevisionId = 0;
	/** 节点大类，例如 BlueprintNode、MaterialNode。 */
	FString NodeType;
	/** 节点显示名。 */
	FString NodeName;
	/** 节点具体类名。 */
	FString NodeClass;
	/** 额外属性，通常存成 JSON。 */
	FString Properties;
	/** 编辑器里的 X 坐标。 */
	int32 PosX = 0;
	/** 编辑器里的 Y 坐标。 */
	int32 PosY = 0;
};

struct FIndexedConnection
{
	/** 连线主键。 */
	int64 Id = 0;
	/** 源节点 id。 */
	int64 SourceNodeId = 0;
	/** 源 pin 名。 */
	FString SourcePin;
	/** 目标节点 id。 */
	int64 TargetNodeId = 0;
	/** 目标 pin 名。 */
	FString TargetPin;
	/** pin 类型字符串。 */
	FString PinType;
};

struct FIndexedVariable
{
	/** 变量行主键。 */
	int64 Id = 0;
	/** 所属资产。 */
	int64 AssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** 变量名。 */
	FString VarName;
	/** 变量类型。 */
	FString VarType;
	/** 分类名。 */
	FString Category;
	/** 默认值文本。 */
	FString DefaultValue;
	/** 是否对外暴露。 */
	bool bIsExposed = false;
	/** 是否网络复制。 */
	bool bIsReplicated = false;
};

struct FIndexedParameter
{
	/** 参数行主键。 */
	int64 Id = 0;
	/** 所属资产。 */
	int64 AssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** 参数名。 */
	FString ParamName;
	/** 参数类型。 */
	FString ParamType;
	/** 参数分组。 */
	FString ParamGroup;
	/** 默认值文本。 */
	FString DefaultValue;
	/** 参数来源，例如 Material、Niagara。 */
	FString Source; // "Material", "Niagara", etc.
};

struct FIndexedDependency
{
	/** 依赖行主键。 */
	int64 Id = 0;
	/** 发出依赖的资产。 */
	int64 SourceAssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** 被依赖的目标资产。 */
	int64 TargetAssetId = 0;
	/** 依赖类型，例如 Hard、Soft。 */
	FString DependencyType; // "Hard", "Soft", "Searchable"
};

struct FIndexedActor
{
	/** Actor 行主键。 */
	int64 Id = 0;
	/** 所属 Level 资产。 */
	int64 AssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** Actor 内部名字。 */
	FString ActorName;
	/** Actor 类名。 */
	FString ActorClass;
	/** 关卡里看到的标签名。 */
	FString ActorLabel;
	/** 变换信息 JSON。 */
	FString Transform;
	/** 组件摘要 JSON。 */
	FString Components;
};

struct FIndexedTag
{
	/** Tag 行主键。 */
	int64 Id = 0;
	/** 完整 tag 名。 */
	FString TagName;
	/** 父 tag 名。 */
	FString ParentTag;
	/** 当前统计到的引用次数。 */
	int32 ReferenceCount = 0;
};

struct FIndexedTagReference
{
	/** tag 引用行主键。 */
	int64 Id = 0;
	/** 指向哪一个 tag。 */
	int64 TagId = 0;
	/** 引用它的资产。 */
	int64 AssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** 在什么语境里引用到它，例如 Variable、Node。 */
	FString Context; // "Variable", "Node", "Component", etc.
};

struct FIndexedGameplayTagSummary
{
	/** 完整 tag 名。 */
	FString TagName;
	/** 父级 tag 名。 */
	FString ParentTag;
	/** 当前“对外可见 revision”里的引用次数。 */
	int64 ReferenceCount = 0;
	/** 引用它的资产包路径列表。 */
	TArray<FString> ReferencingAssets;
};

struct FIndexedConfig
{
	/** 配置行主键。 */
	int64 Id = 0;
	/** 配置文件路径。 */
	FString FilePath;
	/** INI section。 */
	FString Section;
	/** 键名。 */
	FString Key;
	/** 值文本。 */
	FString Value;
};

struct FIndexedCppSymbol
{
	/** C++ 符号行主键。 */
	int64 Id = 0;
	/** 源文件路径。 */
	FString FilePath;
	/** 符号名。 */
	FString SymbolName;
	/** 符号类型，例如 Class、Function。 */
	FString SymbolType; // "Class", "Function", "Enum", "Struct", "Delegate"
	/** 函数签名或声明文本。 */
	FString Signature;
	/** 所在行号。 */
	int32 LineNumber = 0;
	/** 父级符号，例如类内函数的类名。 */
	FString ParentSymbol;
};

struct FIndexedDataTableRow
{
	/** 行记录主键。 */
	int64 Id = 0;
	/** 所属 DataTable 资产。 */
	int64 AssetId = 0;
	/** 所属 revision。 */
	int64 RevisionId = 0;
	/** 行名。 */
	FString RowName;
	/** 行内容 JSON。 */
	FString RowData;
};

struct FIndexedMeshCatalogEntry
{
	/** 目录行主键。 */
	int64 Id = 0;
	/** 属于哪个静态网格资产。 */
	int64 AssetId = 0;
	/** 属于哪个 revision。 */
	int64 RevisionId = 0;
	/** 资产对象路径，例如 /Game/Props/SM_Chair.SM_Chair。 */
	FString AssetPath;
	/** 原始 X 方向完整尺寸。 */
	double BoundsX = 0.0;
	/** 原始 Y 方向完整尺寸。 */
	double BoundsY = 0.0;
	/** 原始 Z 方向完整尺寸。 */
	double BoundsZ = 0.0;
	/** 三个轴排序后的最小值。 */
	double BoundsMin = 0.0;
	/** 三个轴排序后的中间值。 */
	double BoundsMid = 0.0;
	/** 三个轴排序后的最大值。 */
	double BoundsMax = 0.0;
	/** AABB 体积。 */
	double Volume = 0.0;
	/** 粗粒度尺寸分类，例如 tiny / medium / huge。 */
	FString SizeClass;
	/** 从目录层级推出来的类别摘要。 */
	FString Category;
	/** LOD0 三角面数。 */
	int32 TriCount = 0;
	/** 是否存在碰撞设置。 */
	bool bHasCollision = false;
	/** LOD 总数。 */
	int32 LodCount = 0;
	/** pivot 相对包围盒底部的高度偏移。 */
	double PivotOffsetZ = 0.0;
	/** 是否接近退化网格。 */
	bool bDegenerate = false;
};

struct FMonolithAssetIndexMetadata
{
	/** 这份元数据属于哪个资产。 */
	int64 AssetId = 0;
	/** 使用的 indexer id。 */
	FString IndexerId;
	/** indexer 逻辑版本。 */
	uint32 IndexerVersion = 1;
	/** artifact 序列化格式版本。 */
	uint8 ArtifactSchemaVersion = 1;
	/** identity 由谁生成。 */
	FString IdentityProvider;
	/** 运行模式。 */
	FString ExecutionMode;
	/** 最终 identity hash。 */
	FString IdentityHash;
};

struct FMonolithShadowIndexedNode
{
	/** 真正的节点数据。 */
	FIndexedNode Node;
	/** 为了快速比较而算出的行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowNodeAggregate
{
	/** 一共多少行。 */
	uint64 RowCount = 0;
	/** 全部行哈希的求和摘要。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowIndexedParameter
{
	/** 真正的参数数据。 */
	FIndexedParameter Parameter;
	/** 参数行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowIndexedVariable
{
	/** 真正的变量数据。 */
	FIndexedVariable Variable;
	/** 变量行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowIndexedActor
{
	/** 真正的 actor 数据。 */
	FIndexedActor Actor;
	/** actor 行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowIndexedDataTableRow
{
	/** 真正的 DataTable 行。 */
	FIndexedDataTableRow Row;
	/** 行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowParameterAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowVariableAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowActorAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowDataTableRowAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowIndexedDependency
{
	/** 被依赖目标的包路径。 */
	FString TargetPackagePath;
	/** 依赖类型，例如 Hard / Soft。 */
	FString DependencyType;
	/** 为 Level 1 diff 预先算好的稳定哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowDependencyAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowIndexedTagReference
{
	/** 被引用的 GameplayTag 名。 */
	FString TagName;
	/** 引用这个 tag 的语境。 */
	FString Context;
	/** 为 Level 1 diff 预先算好的稳定哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowTagReferenceAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowIndexedConnection
{
	/** 源节点的 shadow 行哈希。 */
	uint64 SourceNodeRowHash = 0;
	/** 源 pin 名。 */
	FString SourcePin;
	/** 目标节点的 shadow 行哈希。 */
	uint64 TargetNodeRowHash = 0;
	/** 目标 pin 名。 */
	FString TargetPin;
	/** pin 类型。 */
	FString PinType;
	/** 整条连线自己的行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowIndexedMeshCatalogEntry
{
	/** 真正的 mesh catalog 行。 */
	FIndexedMeshCatalogEntry Entry;
	/** mesh catalog 行哈希。 */
	uint64 RowHash = 0;
};

struct FMonolithShadowConnectionAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowMeshCatalogAggregate
{
	/** 行数。 */
	uint64 RowCount = 0;
	/** 哈希和。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowTableRetention
{
	/** 哪个 cohort 拥有这份 retention 记录。 */
	FString CohortName;
	/** 这是哪一种基础表，例如 nodes、variables。 */
	FString BaseTableName;
	/** 真实 shadow 表名。 */
	FString TableName;
	/** 到这个时间之后就可以清理。 */
	FDateTime ExpiresAtUtc;
	/** 是否因为 rollback 需要而额外保留。 */
	bool bRollbackRetained = false;
};

struct FSearchResult
{
	/** 资产路径。 */
	FString AssetPath;
	/** 资产短名。 */
	FString AssetName;
	/** 资产类型。 */
	FString AssetClass;
	/** 模块或插件名。 */
	FString ModuleName;
	/** 搜索命中的上下文片段。 */
	FString MatchContext;
	/** 搜索排序分数。 */
	float Rank = 0.0f;
	/** 是否已经陈旧。 */
	bool bStale = false;
};

/*
 * FMonolithIndexDatabase 是对底层 FSQLiteDatabase 的一层“更好用的包装”。
 *
 * 它负责：
 * - 首次打开时建表、补 schema、做迁移；
 * - 提供“按类型读写”的函数，避免业务代码到处手写 SQL；
 * - 把 revision / shadow 这些 Monolith 特有概念收口在数据库层。
 *
 * 线程模型上，它更偏向“单写多读”：
 * - 读接口本身尽量轻量；
 * - 写操作仍然应该由上层按顺序串起来。
 */
class MONOLITHINDEX_API FMonolithIndexDatabase
{
public:
	FMonolithIndexDatabase();
	~FMonolithIndexDatabase();

	/** 打开或创建数据库文件。 */
	bool Open(const FString& InDbPath);
	/** 只读打开数据库文件，供 MCP 后台查询使用。 */
	/** 以“查询专用”模式打开现有数据库。
	 *
	 * 这里故意不用 SQLite 的原生 ReadOnly 打开模式。
	 * 在 Windows 上，Monolith 的 writer 持锁期间用 ReadOnly 新开连接，
	 * 很容易直接在 sqlite3_open 阶段返回 I/O / busy 失败。
	 *
	 * 正确做法是：
	 * - 先用 ReadWrite 打开现有库文件；
	 * - 再强制 `PRAGMA journal_mode=DELETE`；
	 * - 再强制 `PRAGMA query_only=ON`；
	 *
	 * 这样查询连接仍然不会执行写入，但能避开只读打开模式的已知问题。 */
	bool OpenQueryOnly(const FString& InDbPath);

	/** 关闭数据库连接。 */
	void Close();

	/** 当前是否已经成功打开数据库。 */
	bool IsOpen() const;

	/** 清空所有数据并重建表结构，用于 full index。 */
	bool ResetDatabase();

	/** 直接拿到底层 SQLite 句柄。
	 * 一般只在少数底层辅助逻辑里用，正常业务更推荐走 typed helper。 */
	FSQLiteDatabase* GetRawDatabase() const { return Database; }

	// --- Transaction helpers ---
	/** 开启事务，让后续多条 SQL 成为“一起成功、一起失败”的整体。 */
	bool BeginTransaction();
	/** 提交事务。 */
	bool CommitTransaction();
	/** 回滚事务。 */
	bool RollbackTransaction();
	/** 给某个资产开启“写新 revision，但先不对外可见”的阶段。 */
	bool BeginAssetRevisionWrite(int64 AssetId);
	/** 把刚写好的 revision promote 成当前可见版本。 */
	bool CommitAssetRevisionWrite(int64 AssetId);
	/** 放弃这次 revision 写入，保留旧版本继续可见。 */
	void DiscardAssetRevisionWrite(int64 AssetId);

	// --- Asset CRUD ---
	/** 插入或更新资产基础行。 */
	int64 InsertAsset(const FIndexedAsset& Asset);
	/** 按包路径拿资产。 */
	TOptional<FIndexedAsset> GetAssetByPath(const FString& PackagePath);
	/** 只拿资产 id，不取整行。 */
	int64 GetAssetId(const FString& PackagePath);
	/** 删除资产以及它挂着的所有子数据。 */
	bool DeleteAssetAndRelated(int64 AssetId);

	// --- Node CRUD ---
	/** 插入一条节点记录。 */
	int64 InsertNode(const FIndexedNode& Node);
	/** 读取某个资产当前可见 revision 的全部节点。 */
	TArray<FIndexedNode> GetNodesForAsset(int64 AssetId);

	// --- Connection CRUD ---
	/** 插入一条连线记录。 */
	int64 InsertConnection(const FIndexedConnection& Conn);
	/** 读取某个资产的全部连线。 */
	TArray<FIndexedConnection> GetConnectionsForAsset(int64 AssetId);
	/** 用稳定 node-hash 语义读取 production 连线，供 shadow Level 2 diff 复用。 */
	TArray<FMonolithShadowIndexedConnection> GetProductionConnectionsForAsset(int64 AssetId);

	// --- Variable CRUD ---
	/** 插入一条变量记录。 */
	int64 InsertVariable(const FIndexedVariable& Var);
	/** 读取某个资产的全部变量。 */
	TArray<FIndexedVariable> GetVariablesForAsset(int64 AssetId);

	// --- Parameter CRUD ---
	/** 插入一条参数记录。 */
	int64 InsertParameter(const FIndexedParameter& Param);
	/** 读取某个资产的全部参数。 */
	TArray<FIndexedParameter> GetParametersForAsset(int64 AssetId);

	// --- Dependency CRUD ---
	/** 插入一条依赖边。 */
	int64 InsertDependency(const FIndexedDependency& Dep);
	/** 读取“这个资产依赖了谁”。 */
	TArray<FIndexedDependency> GetDependenciesForAsset(int64 AssetId);
	/** 用“目标包路径 + 依赖类型”语义读取 production dependencies，供 shadow Level 2 diff 复用。 */
	TArray<FMonolithShadowIndexedDependency> GetProductionDependenciesForAsset(int64 AssetId);
	/** 读取“谁依赖了这个资产”。 */
	TArray<FIndexedDependency> GetReferencersOfAsset(int64 AssetId);
	/** 清空依赖表，用于某些旧路径或重建场景。 */
	bool ClearDependencies();

	// --- Actor CRUD ---
	/** 插入一条关卡 actor 记录。 */
	int64 InsertActor(const FIndexedActor& Actor);
	/** 读取某个 Level 资产的 actor 快照。 */
	TArray<FIndexedActor> GetActorsForAsset(int64 AssetId);

	// --- Tag CRUD ---
	/** 插入一条 GameplayTag 定义。 */
	int64 InsertTag(const FIndexedTag& Tag);
	/** 没有就创建，有就返回已有 id。 */
	int64 GetOrCreateTag(const FString& TagName, const FString& ParentTag);
	/** 插入一条 tag 引用。 */
	int64 InsertTagReference(const FIndexedTagReference& Ref);
	/** 用“tag 名 + context”语义读取 production tag references，供 shadow Level 2 diff 复用。 */
	TArray<FMonolithShadowIndexedTagReference> GetProductionTagReferencesForAsset(int64 AssetId);
	/** 清空 tag 定义和引用，用于重建。 */
	bool ClearGameplayTagIndex();
	/** 列出 tag 清单，可按前缀过滤。 */
	TArray<FIndexedGameplayTagSummary> ListGameplayTags(const FString& Prefix);
	/** 按子串搜索 tag，并返回引用它们的资产路径。 */
	TArray<FIndexedGameplayTagSummary> SearchGameplayTags(const FString& Query);

	// --- Meta ---
	/** 写入元信息键值对。 */
	bool WriteMeta(const FString& Key, const FString& Value);
	/** 读取元信息键值对。 */
	FString ReadMeta(const FString& Key) const;
	/** 判断数据库里是否至少已经存在一份资产快照。 */
	bool HasIndexedAssetSnapshot() const;

	// --- Config CRUD ---
	/** 插入一条配置项。 */
	int64 InsertConfig(const FIndexedConfig& Config);
	/** 清空全部配置项，用于全局 artifact 回放。 */
	bool ClearConfigIndex();

	// --- C++ Symbol CRUD ---
	/** 插入一条 C++ 符号。 */
	int64 InsertCppSymbol(const FIndexedCppSymbol& Symbol);
	/** 清空全部 C++ 符号，用于全局 artifact 回放。 */
	bool ClearCppSymbolIndex();

	// --- DataTable Row CRUD ---
	/** 插入一条 DataTable 行。 */
	int64 InsertDataTableRow(const FIndexedDataTableRow& Row);
	/** 读取某个 DataTable 资产的当前行快照。 */
	TArray<FIndexedDataTableRow> GetDataTableRowsForAsset(int64 AssetId);

	// --- Mesh catalog CRUD ---
	/** 插入一条 mesh catalog 目录行。 */
	int64 InsertMeshCatalogEntry(const FIndexedMeshCatalogEntry& Entry);
	/** 读取某个静态网格资产当前可见的目录行。 */
	TOptional<FIndexedMeshCatalogEntry> GetMeshCatalogEntryForAsset(int64 AssetId);
	/** 读取当前活动 revision 下所有 mesh catalog 行，可选按路径子串过滤。 */
	TArray<FIndexedMeshCatalogEntry> GetMeshCatalogEntries(const FString& PathFilter = FString());
	/** 按尺寸范围搜索 mesh catalog。 */
	TSharedPtr<FJsonObject> SearchMeshCatalogBySize(
		const TArray<float>& MinBounds,
		const TArray<float>& MaxBounds,
		const FString& Category,
		const FString& ExcludeSizeClass,
		int32 Limit);
	/** 获取 mesh catalog 聚合统计。 */
	TSharedPtr<FJsonObject> GetMeshCatalogStats();

	// --- Mesh visual CRUD（双 cohort 共享同套接口，按 CohortName 落到不同物理表）---
	/** 把一行视觉 cohort 数据写入 production 表。CohortName 必须是 `AssetVisualGeometric` 或 `AssetVisualSemantic`。 */
	int64 InsertAssetVisualEntry(const FString& CohortName, const FIndexedAssetVisualEntry& Entry);
	/** 清空整张视觉 cohort production 表（不删 shadow）。Materialize 重跑前先清，避免 INSERT 累加。 */
	bool ClearAssetVisualEntries(const FString& CohortName);
	/** 读取某资产当前可见 revision 下的视觉行；不存在时返回空。
	 *  Multi-phase 资产同 asset 在 cohort 内有多行，此 API 按 phase_id ASC 取第一行（即 phase 0）；
	 *  需要全部 phase 行调 GetAssetVisualEntriesForAsset。 */
	TOptional<FIndexedAssetVisualEntry> GetAssetVisualEntryForAsset(const FString& CohortName, int64 AssetId);
	/** 读取某资产当前可见 revision 下所有 phase 的视觉行（按 phase_id ASC 排序）。
	 *  单 phase 资产返回长度 1；多 phase 资产返回长度 N（与 GetPhasesForAsset 对应）。 */
	TArray<FIndexedAssetVisualEntry> GetAssetVisualEntriesForAsset(const FString& CohortName, int64 AssetId);
	/** 读取整张视觉 cohort 表，可选按 ShardId 过滤；用于 shard reducer 重建 ANN 快照。 */
	TArray<FIndexedAssetVisualEntry> GetAssetVisualEntries(const FString& CohortName, const FString& ShardIdFilter = FString());
	/** 用一整份新快照替换某资产在视觉 shadow 表里的行。 */
	bool ReplaceShadowAssetVisualEntriesForAsset(
		const FString& CohortName,
		const FString& ShadowCohortName,
		int64 AssetId,
		const TArray<FMonolithShadowIndexedAssetVisualEntry>& Entries);
	/** 读取某资产当前可见 revision 下的视觉 shadow 行。 */
	TArray<FMonolithShadowIndexedAssetVisualEntry> GetShadowAssetVisualEntriesForAsset(
		const FString& CohortName,
		const FString& ShadowCohortName,
		int64 AssetId);
	/** 视觉 production 行的 Level 1 聚合摘要。 */
	FMonolithShadowAssetVisualAggregate GetProductionAssetVisualAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 视觉 shadow 行的 Level 1 聚合摘要。 */
	FMonolithShadowAssetVisualAggregate GetShadowAssetVisualAggregateForAsset(
		const FString& CohortName,
		const FString& ShadowCohortName,
		int64 AssetId);

	// --- Index metadata ---
	/** 更新资产对应的索引元数据。 */
	bool UpsertAssetIndexMetadata(const FMonolithAssetIndexMetadata& Metadata);
	/** 按资产 id 读取索引元数据。 */
	TOptional<FMonolithAssetIndexMetadata> GetAssetIndexMetadataByAssetId(int64 AssetId);
	/** 一次性把整张 asset_index_metadata 表读出来，按 asset_id 建索引返回。
	 *  循环里大量调 GetAssetIndexMetadataByAssetId 时（如 GatherKnownStalePackages 33K+ 资产场景）
	 *  请用这个，避免 N 次 prepared statement 创建/绑定/拆销造成 GT 卡顿。 */
	TMap<int64, FMonolithAssetIndexMetadata> GetAllAssetIndexMetadata();
	/** 读取所有资产。 */
	TArray<FIndexedAsset> GetAllAssets();

	// --- Shadow tables ---
	/** 用一整份新快照替换某资产的 shadow nodes。 */
	bool ReplaceShadowNodesForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedNode>& Nodes);
	/** 读取某个资产当前可见 revision 的 shadow nodes。 */
	TArray<FMonolithShadowIndexedNode> GetShadowNodesForAsset(const FString& CohortName, int64 AssetId);
	/** 统计正式生产表里的节点聚合摘要。 */
	FMonolithShadowNodeAggregate GetProductionNodeAggregateForAsset(int64 AssetId);
	/** 统计 shadow 表里的节点聚合摘要。 */
	FMonolithShadowNodeAggregate GetShadowNodeAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow variables。 */
	bool ReplaceShadowVariablesForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedVariable>& Variables);
	/** 读取某个资产当前可见 revision 的 shadow variables。 */
	TArray<FMonolithShadowIndexedVariable> GetShadowVariablesForAsset(const FString& CohortName, int64 AssetId);
	/** 正式变量聚合摘要。 */
	FMonolithShadowVariableAggregate GetProductionVariableAggregateForAsset(int64 AssetId);
	/** shadow 变量聚合摘要。 */
	FMonolithShadowVariableAggregate GetShadowVariableAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow actors。 */
	bool ReplaceShadowActorsForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedActor>& Actors);
	/** 读取某个资产当前可见 revision 的 shadow actors。 */
	TArray<FMonolithShadowIndexedActor> GetShadowActorsForAsset(const FString& CohortName, int64 AssetId);
	/** 正式 actor 聚合摘要。 */
	FMonolithShadowActorAggregate GetProductionActorAggregateForAsset(int64 AssetId);
	/** shadow actor 聚合摘要。 */
	FMonolithShadowActorAggregate GetShadowActorAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow DataTable 行。 */
	bool ReplaceShadowDataTableRowsForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedDataTableRow>& Rows);
	/** 读取某个资产当前可见 revision 的 shadow DataTable 行。 */
	TArray<FMonolithShadowIndexedDataTableRow> GetShadowDataTableRowsForAsset(const FString& CohortName, int64 AssetId);
	/** 正式 DataTable 行聚合摘要。 */
	FMonolithShadowDataTableRowAggregate GetProductionDataTableRowAggregateForAsset(int64 AssetId);
	/** shadow DataTable 行聚合摘要。 */
	FMonolithShadowDataTableRowAggregate GetShadowDataTableRowAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow dependencies。 */
	bool ReplaceShadowDependenciesForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedDependency>& Dependencies);
	/** 读取某个资产当前可见 revision 的 shadow dependencies。 */
	TArray<FMonolithShadowIndexedDependency> GetShadowDependenciesForAsset(const FString& CohortName, int64 AssetId);
	/** 正式 dependency 聚合摘要。 */
	FMonolithShadowDependencyAggregate GetProductionDependencyAggregateForAsset(int64 AssetId);
	/** shadow dependency 聚合摘要。 */
	FMonolithShadowDependencyAggregate GetShadowDependencyAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow tag references。 */
	bool ReplaceShadowTagReferencesForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedTagReference>& References);
	/** 读取某个资产当前可见 revision 的 shadow tag references。 */
	TArray<FMonolithShadowIndexedTagReference> GetShadowTagReferencesForAsset(const FString& CohortName, int64 AssetId);
	/** 正式 tag reference 聚合摘要。 */
	FMonolithShadowTagReferenceAggregate GetProductionTagReferenceAggregateForAsset(int64 AssetId);
	/** shadow tag reference 聚合摘要。 */
	FMonolithShadowTagReferenceAggregate GetShadowTagReferenceAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow 参数。 */
	bool ReplaceShadowParametersForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedParameter>& Parameters);
	/** 读取某个资产当前可见 revision 的 shadow parameters。 */
	TArray<FMonolithShadowIndexedParameter> GetShadowParametersForAsset(const FString& CohortName, int64 AssetId);
	/** 正式参数聚合摘要。 */
	FMonolithShadowParameterAggregate GetProductionParameterAggregateForAsset(int64 AssetId);
	/** shadow 参数聚合摘要。 */
	FMonolithShadowParameterAggregate GetShadowParameterAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow 连线。 */
	bool ReplaceShadowConnectionsForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedConnection>& Connections);
	/** 读取某个资产当前可见 revision 的 shadow connections。 */
	TArray<FMonolithShadowIndexedConnection> GetShadowConnectionsForAsset(const FString& CohortName, int64 AssetId);
	/** 正式连线聚合摘要。 */
	FMonolithShadowConnectionAggregate GetProductionConnectionAggregateForAsset(int64 AssetId);
	/** shadow 连线聚合摘要。 */
	FMonolithShadowConnectionAggregate GetShadowConnectionAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 用一整份新快照替换 shadow mesh catalog 行。 */
	bool ReplaceShadowMeshCatalogEntriesForAsset(const FString& CohortName, int64 AssetId, const TArray<FMonolithShadowIndexedMeshCatalogEntry>& Entries);
	/** 读取某个资产当前可见 revision 的 shadow mesh catalog 行。 */
	TArray<FMonolithShadowIndexedMeshCatalogEntry> GetShadowMeshCatalogEntriesForAsset(const FString& CohortName, int64 AssetId);
	/** 正式 mesh catalog 聚合摘要。 */
	FMonolithShadowMeshCatalogAggregate GetProductionMeshCatalogAggregateForAsset(int64 AssetId);
	/** shadow mesh catalog 聚合摘要。 */
	FMonolithShadowMeshCatalogAggregate GetShadowMeshCatalogAggregateForAsset(const FString& CohortName, int64 AssetId);
	/** 记录某张 shadow 表要保留到什么时候。 */
	bool UpsertShadowTableRetention(const FString& CohortName, const FString& BaseTableName, const FDateTime& ExpiresAtUtc, bool bRollbackRetained);
	/** 读取所有 shadow retention 记录。 */
	TArray<FMonolithShadowTableRetention> GetShadowTableRetentions();
	/** 清理已经过期的 shadow 表，返回删掉的表数量。 */
	int32 DropExpiredShadowTables(const FDateTime& NowUtc);

	// --- FTS5 Search ---
	/** 全文搜索入口。 */
	TArray<FSearchResult> FullTextSearch(const FString& Query, int32 Limit = 50);

	// --- Stats ---
	/** 生成统计信息 JSON。 */
	TSharedPtr<FJsonObject> GetStats();

	// --- Asset details (all related data) ---
	/** 生成某个资产的完整详情 JSON。 */
	TSharedPtr<FJsonObject> GetAssetDetails(const FString& PackagePath);

	// --- Find by type ---
	/** 按资产类型分页查询。 */
	TArray<FIndexedAsset> FindByType(const FString& AssetClass, int32 Limit = 100, int32 Offset = 0);

	// --- Find references (bidirectional) ---
	/** 查询依赖和被依赖两侧关系。 */
	TSharedPtr<FJsonObject> FindReferences(const FString& PackagePath);

	// --- Incremental indexing helpers ---
	/** 读取所有已经索引过的路径。 */
	TArray<FString> GetAllIndexedPaths();
	/** 读取某个资产上次记录的保存哈希。 */
	FString GetSavedHash(const FString& PackagePath);
	/** 一次性拿到所有路径和哈希。 */
	TMap<FString, FString> GetAllPathsAndHashes();
	/** 按路径删除资产。 */
	bool DeleteAssetByPath(const FString& PackagePath);
	/** 资产重命名时更新路径和名字。 */
	bool UpdateAssetPath(const FString& OldPath, const FString& NewPath, const FString& NewAssetName = FString());
	/** 更新资产基础信息。 */
	bool UpdateAssetMetadata(const FIndexedAsset& Asset);
	/** 删除资产的子表数据，不删资产主行。 */
	bool DeleteChildDataForAsset(int64 AssetId);
	/** 只更新保存哈希。 */
	bool UpdateSavedHash(const FString& PackagePath, const FString& HashHex);

private:
	/** 查询时应该看到哪个 revision。 */
	int64 ResolveActiveRevisionId(int64 AssetId) const;
	/** 写入时应该落到哪个 revision；如果当前没开新 revision，就回退到现有 revision。 */
	int64 ResolveWriteOrCurrentRevisionId(int64 AssetId) const;
	/** promote 成功后删除已经被替代的旧 revision 子数据。 */
	bool DeleteSupersededAssetRevisionRows(int64 AssetId, int64 KeepRevisionId);
	/** 确保某个 cohort 的 shadow nodes 表存在。 */
	bool EnsureShadowNodesTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow variables 表存在。 */
	bool EnsureShadowVariablesTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow actors 表存在。 */
	bool EnsureShadowActorsTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow DataTableRows 表存在。 */
	bool EnsureShadowDataTableRowsTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow dependencies 表存在。 */
	bool EnsureShadowDependenciesTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow tag references 表存在。 */
	bool EnsureShadowTagReferencesTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow parameters 表存在。 */
	bool EnsureShadowParametersTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow connections 表存在。 */
	bool EnsureShadowConnectionsTable(const FString& CohortName);
	/** 确保某个 cohort 的 shadow mesh catalog 表存在。 */
	bool EnsureShadowMeshCatalogTable(const FString& CohortName);
	/** 首次打开数据库时建表。 */
	bool CreateTables();
	/** 执行一段原始 SQL。Context 只用于日志定位。 */
	bool ExecuteSQL(const FString& SQL, const TCHAR* Context = nullptr);
	/** SQLite 连接本体。 */
	FSQLiteDatabase* Database = nullptr;
	/** 数据库文件路径。 */
	FString DbPath;
	/** 资产当前正在写但尚未 promote 的 revision 记录。 */
	TMap<int64, int64> ActiveAssetRevisions;
};
