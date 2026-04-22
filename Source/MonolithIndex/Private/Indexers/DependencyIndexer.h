#pragma once

#include "MonolithIndexer.h"

/*
 * FDependencyIndexer 负责把 Asset Registry 里的依赖关系写进 Monolith 数据库。
 *
 * 现在它只保留“单资产 companion”这一套实现：
 * - 对任意真实 package 资产，都可以提取出它引用了哪些包；
 * - 生产表 / artifact / shadow 都复用同一份 payload；
 * - 不再保留 fake sentinel class，也不再保留 scoped sentinel 旁路。
 *
 * 这样主资产 promote 时，依赖边会跟着一起切换，不会再出现新旧混看的状态。
 */
class FDependencyIndexer : public IMonolithIndexer
{
public:
	/** companion 不通过真实资产类分发表命中，所以这里返回空列表。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {};
	}
	/** Dependency 是“全资产伴生”型 indexer。
	 * 它不会占用主 class dispatch，但对任何真实 package 资产都应该生效。 */
	virtual bool MatchesAsset(const FAssetData& AssetData, const UObject* LoadedAsset = nullptr) const override
	{
		(void)LoadedAsset;
		return !AssetData.PackageName.IsNone();
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("DependencyIndexer"); }
	/** 用更短、更稳定的 cohort/id 名字参与 artifact identity 和 shadow mode。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Dependency")); }
	/** 依赖边只需要 Asset Registry，不必真正加载 UObject。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::AROnly; }
	/** 把单资产 dependency 快照打包成 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 dependency artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 dependency artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
