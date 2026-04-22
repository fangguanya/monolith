#include "MonolithIndexer.h"

#include "AssetRegistry/AssetRegistryModule.h"

/*
 * 这里实现 IMonolithIndexer 的默认 package-scoped 索引主链。
 *
 * 目标只有一个：
 * - 把“BuildArtifact -> MaterializeArtifact”收成唯一入口；
 * - 让各个具体 indexer 不必再各自保留一份同样的转发样板。
 *
 * 这样以后新增或重构 package indexer 时，只需要关心：
 * 1. 怎么构建稳定 artifact；
 * 2. 怎么回放 artifact。
 *
 * 至于“先构建再回放”的调度壳，统一由这里负责。
 */

bool IMonolithIndexer::IndexAsset(
	const FAssetData& AssetData,
	UObject* LoadedAsset,
	FMonolithIndexDatabase& DB,
	const int64 AssetId)
{
	// GlobalReducer 不对应单个包资产。
	// 如果有人通过统一入口调到这里，直接转发到全局索引入口即可。
	if (GetExecutionMode() == EMonolithExecutionMode::GlobalReducer)
	{
		(void)AssetData;
		(void)LoadedAsset;
		(void)AssetId;
		return IndexGlobal(DB);
	}

	// 包级 indexer 一律走 artifact 主链，不再保留平行的“直接写生产表”实现。
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FMonolithArtifact Artifact;
	if (!BuildArtifact(AssetData, LoadedAsset, AssetRegistry, Artifact))
	{
		return false;
	}

	return MaterializeArtifact(Artifact, DB, AssetId);
}
