#pragma once

#include "MonolithIndexer.h"

namespace MonolithSimpleArtifactSerialization
{
	struct FGraphPayload;
}

/*
 * FEQSIndexer 负责把 EnvQuery 资产整理成真正的内部查询图：
 * - Option 节点
 * - Generator 节点
 * - Test 节点
 * - 它们之间的内部连线
 *
 * 旧实现是 sentinel，而且主要只留下“用了哪些类”的弱语义。
 * 这轮改成 package-scoped 后，shadow diff 就能比较真正的查询结构。
 */
class FEQSIndexer : public IMonolithIndexer
{
public:
	/** 只处理真实的 EnvQuery 资产类。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("EnvQuery") };
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("EQSIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("EQS")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 把 EnvQuery 整理成稳定 graph payload。 */
	bool BuildPayload(class UEnvQuery* Query, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const;
};
