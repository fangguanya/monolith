#pragma once

#include "MonolithIndexer.h"

/*
 * FGASIndexer 负责提取 Gameplay Ability System 相关 Blueprint 资产的“业务语义层”摘要。
 *
 * 它和 BlueprintIndexer 的分工不同：
 * - BlueprintIndexer 更像“图结构扫描器”，关注蓝图里有哪些图节点和引脚连线；
 * - GASIndexer 更像“玩法配置扫描器”，关注 Ability / Effect / AttributeSet / Cue 的规则字段。
 *
 * 这轮改造后，它不再是一个自己全项目重扫的 sentinel，
 * 而是变成真正的 per-asset companion：
 * - 只在命中的 GAS Blueprint 上运行；
 * - 跟随主资产 revision 一起提交；
 * - 不再保留那条没有纳入主链的旧全局扫描实现。
 *
 * 这里暂时只处理“项目里的包资产”：
 * - GameplayAbility Blueprint
 * - GameplayEffect Blueprint
 * - AttributeSet Blueprint
 * - GameplayCue Blueprint
 *
 * 原先 sentinel 里那种“顺手把所有原生 C++ AttributeSet 类也扫一遍”的行为，
 * 本质上更接近 Cpp/GlobalReducer 范畴，不属于 package-scoped companion 的同一套语义，
 * 所以这里明确不再混在一起。
 */
class FGASIndexer : public IMonolithIndexer
{
public:
	/** GAS companion 挂靠在 Blueprint 资产上。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("Blueprint") };
	}

	/** 只有真正继承 GAS 基类的 Blueprint 才应该命中这个 indexer。 */
	virtual bool MatchesAsset(const FAssetData& AssetData, const UObject* LoadedAsset = nullptr) const override;
	/** 为单个 GAS Blueprint 写入语义摘要。
	 *
	 * 这条生产写入路径现在故意不再自己维护一套“直接写数据库”的逻辑，
	 * 而是复用下面的 artifact 构建和回放步骤。
	 *
	 * 这样可以保证：
	 * - 正式表写入；
	 * - artifact 缓存；
	 * - shadow 表写入；
	 *
	 * 三条链路都来自同一份节点快照，不会出现三套实现慢慢漂移的问题。
	 */
	/** 把单个 GAS Blueprint 的语义摘要打包成 artifact。
	 *
	 * 这里产出的 payload 很简单：
	 * - 只有 1 条 node；
	 * - node 里放的是这个 GAS 资产的结构化 JSON 摘要。
	 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 GAS artifact 回放到正式生产表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 GAS artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("GASIndexer"); }
	/** 稳定 cohort/id 名称。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("GAS")); }

private:
	/** 根据 Blueprint 的真实 GAS 类型，统一构建出那 1 条节点快照。 */
	bool BuildNodeForAsset(const FAssetData& AssetData, class UBlueprint* Blueprint, FIndexedNode& OutNode) const;
	/** 为 GameplayAbility Blueprint 生成 1 条 node。 */
	bool BuildGameplayAbilityNode(const FAssetData& AssetData, class UBlueprint* Blueprint, FIndexedNode& OutNode) const;
	/** 为 GameplayEffect Blueprint 生成 1 条 node。 */
	bool BuildGameplayEffectNode(const FAssetData& AssetData, class UBlueprint* Blueprint, FIndexedNode& OutNode) const;
	/** 为 AttributeSet Blueprint 生成 1 条 node。 */
	bool BuildAttributeSetNode(const FAssetData& AssetData, class UBlueprint* Blueprint, FIndexedNode& OutNode) const;
	/** 为 GameplayCue Blueprint 生成 1 条 node。 */
	bool BuildGameplayCueNode(const FAssetData& AssetData, class UBlueprint* Blueprint, FIndexedNode& OutNode) const;
};
