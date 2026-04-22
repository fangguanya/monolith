#pragma once

// StateTree 仅在引擎包含 StateTreeModule 时可用
#if WITH_STATETREE

#include "MonolithIndexer.h"

namespace MonolithSimpleArtifactSerialization
{
	struct FGraphPayload;
}

/*
 * FStateTreeIndexer 把 UStateTree 转成真正的状态图快照：
 * - State 节点
 * - Task 节点
 * - State -> State 的 transition
 * - State -> Task 的附属关系
 *
 * 旧实现是 sentinel，而且主要保留“状态列表 + 任务类名”。
 * 现在改成 package-scoped 后，shadow diff 能比较真正的状态机结构。
 */
class FStateTreeIndexer : public IMonolithIndexer
{
public:
	/** 只处理真实的 StateTree 资产类。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("StateTree") };
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("StateTreeIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("StateTree")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 把 StateTree 运行时对象整理成 graph payload。 */
	bool BuildPayload(class UStateTree* StateTree, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const;
};

#endif // WITH_STATETREE
