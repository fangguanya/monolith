#pragma once

#include "MonolithIndexer.h"

/*
 * FGameplayTagIndexer 现在只负责“各个资产对 GameplayTag 的引用”。
 *
 * 全局定义树已经拆到独立的 reducer indexer，
 * 这样这个 companion indexer 就可以专心处理：
 * - 每个资产引用了哪些 tag；
 * - 这些引用如何跟随 asset revision 一起切换；
 * - 生产表 / artifact / shadow 如何共享同一份 payload。
 *
 * 它现在不再保留 scoped sentinel 旁路，
 * 只保留 companion 这一套 per-asset 实现。
 */
class FGameplayTagIndexer : public IMonolithIndexer
{
public:
	/** companion 不通过真实资产类分发表命中，所以这里返回空列表。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {};
	}
	/** GameplayTag 引用同样是“全资产伴生”型数据。
	 * 只要它是一份真实 package 资产，我们都应该允许尝试提取 tag 引用。 */
	virtual bool MatchesAsset(const FAssetData& AssetData, const UObject* LoadedAsset = nullptr) const override
	{
		(void)LoadedAsset;
		return !AssetData.PackageName.IsNone();
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("GameplayTagIndexer"); }
	/** 用更贴近业务语义的 id 名字参与 warmup / shadow / identity。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("GameplayTags")); }
	/** tag 引用主要来自 Asset Registry 元数据，不需要真正加载 UObject。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::AROnly; }
	/** 把单资产 tag 引用快照打包成 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 tag 引用 artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 tag 引用 artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
