#pragma once

#include "MonolithIndexer.h"

/*
 * FInputActionIndexer 负责 Enhanced Input 的 InputAction 资产。
 *
 * 这类资产的关键信息通常很集中：
 * - 值类型
 * - 是否消费输入
 * - Trigger / Modifier 列表
 *
 * 所以它适合做成一条轻量 node 摘要。
 */
class FInputActionIndexer : public IMonolithIndexer
{
public:
	/** 这个 indexer 只接管 Enhanced Input 的 InputAction 资产。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("InputAction") };
	}

	/** 用于 artifact/shadow/warmup 的稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("InputAction")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("InputActionIndexer"); }
	/** 构建可缓存 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
