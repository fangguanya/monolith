#pragma once

#include "CoreMinimal.h"
#include "MonolithIndexDatabase.h"

/*
 * 这份头文件提供的是 shadow mode 的“公共小工具”。
 *
 * shadow mode 可以简单理解成：
 * - 生产表继续服务真实查询；
 * - 新链路先把结果写进 shadow_* 表；
 * - 然后比较两边是否足够接近，再决定 promote 还是 rollback。
 *
 * 这里声明的函数主要分三类：
 * 1. 表名和 retention 相关的小工具；
 * 2. Level 1 / Level 2 diff 的聚合判断；
 * 3. 为不同类型行计算稳定 row_hash 的函数。
 *
 * “稳定”很重要，意思是：
 * 只要业务内容一样，就算 asset_id / revision_id 不一样，hash 也应该一样。
 */

struct FMonolithShadowAggregate
{
	/** 一共有多少行参与这次聚合。 */
	uint64 RowCount = 0;
	/** 把每一行的稳定哈希加总后的结果。 */
	uint64 RowHashSum = 0;
};

struct FMonolithShadowDiffDecision
{
	/** 估算出来的差异比例，越大表示越不像。 */
	double AggregateDifferenceRatio = 0.0;
	/** 是否值得继续做更贵的 Level 2 逐行比较。 */
	bool bRequiresLevel2 = false;
	/** 差异太大时，是否应该把这次 shadow 结果判成回滚候选。 */
	bool bShouldRollback = false;
};

/*
 * Level 2 diff 不再只看“总数和哈希和”，
 * 而是拿一小部分确定性采样的主键做逐行比较。
 *
 * 这里的 PrimaryKey 约定是：
 * - 调用方先用“业务主键 + 重复序号”拼成稳定 key；
 * - 同一行在 production / shadow 两边必须生成完全相同的 key；
 * - 采样器只基于这个 key 做 1% 稳定抽样。
 */
struct FMonolithShadowLevel2Row
{
	/** 用来做确定性采样和逐行对齐的稳定主键。 */
	FString PrimaryKey;
	/** 这行内容的稳定 row_hash。 */
	uint64 RowHash = 0;
	/** 仅用于日志和排查的可读摘要。 */
	FString DebugSummary;
};

struct FMonolithShadowLevel2DiffResult
{
	/** production 侧被采样命中的行数。 */
	uint32 SampledProductionRows = 0;
	/** shadow 侧被采样命中的行数。 */
	uint32 SampledShadowRows = 0;
	/** 两边都命中且能逐行比较的样本数。 */
	uint32 ComparedRows = 0;
	/** 同 key 下 row_hash 不一致的样本数。 */
	uint32 MismatchedRows = 0;
	/** 只存在于 production 样本中的行数。 */
	uint32 ProductionOnlyRows = 0;
	/** 只存在于 shadow 样本中的行数。 */
	uint32 ShadowOnlyRows = 0;

	/** Level 2 只要出现任意 mismatch / 缺行，就说明样本不一致。 */
	bool HasMismatch() const
	{
		return MismatchedRows > 0 || ProductionOnlyRows > 0 || ShadowOnlyRows > 0;
	}
};

/** 解析配置里的单个 cohort 名。 */
bool ParseShadowModeCohort(const FString& ConfigValue, FName& OutCohortName);
/** 统一生成 shadow 表名，并清洗不安全字符。 */
FString MakeShadowTableName(const FString& CohortName, const FString& BaseTableName);
/** 计算 shadow 表记录的保留截止时间。 */
FDateTime GetShadowRetentionDeadlineUtc(const FDateTime& NowUtc, bool bRollbackRetained);
/** 用一级聚合结果快速判断“新旧结果差得有多远”。 */
FMonolithShadowDiffDecision EvaluateShadowDiff(
	const FMonolithShadowAggregate& Production,
	const FMonolithShadowAggregate& Shadow,
	double RollbackThresholdRatio = 0.001);
/** 用主键做稳定抽样，给 Level 2 逐行 diff 挑样本。 */
bool IsDeterministicLevel2Sample(const FString& PrimaryKey);
/** 对确定性采样后的逐行样本做 Level 2 diff。 */
FMonolithShadowLevel2DiffResult EvaluateShadowLevel2Diff(
	const TArray<FMonolithShadowLevel2Row>& ProductionRows,
	const TArray<FMonolithShadowLevel2Row>& ShadowRows);
/** 计算 node 行的稳定哈希。 */
uint64 ComputeNodeRowHash(const FIndexedNode& Node);
/** 计算 variable 行的稳定哈希。 */
uint64 ComputeVariableRowHash(const FIndexedVariable& Variable);
/** 计算 actor 行的稳定哈希。 */
uint64 ComputeActorRowHash(const FIndexedActor& Actor);
/** 计算 DataTable 行的稳定哈希。 */
uint64 ComputeDataTableRowHash(const FIndexedDataTableRow& Row);
/** 用“目标包路径 + 依赖类型”计算 dependency 行哈希。 */
uint64 ComputeDependencyRowHash(const FString& TargetPackagePath, const FString& DependencyType);
/** 用“tag 名 + 引用语境”计算 tag reference 行哈希。 */
uint64 ComputeTagReferenceRowHash(const FString& TagName, const FString& Context);
/** 计算 parameter 行的稳定哈希。 */
uint64 ComputeParameterRowHash(const FIndexedParameter& Parameter);
/** 计算 mesh catalog 行的稳定哈希。 */
uint64 ComputeMeshCatalogRowHash(const FIndexedMeshCatalogEntry& Entry);
/** 用两端节点哈希和 pin 信息计算 connection 行哈希。 */
uint64 ComputeConnectionRowHash(
	uint64 SourceNodeRowHash,
	const FString& SourcePin,
	uint64 TargetNodeRowHash,
	const FString& TargetPin,
	const FString& PinType);
