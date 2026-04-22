#pragma once

#include "MonolithIndexer.h"

namespace MonolithSimpleArtifactSerialization
{
	struct FGraphPayload;
}

/*
 * FBehaviorTreeIndexer 现在负责两类 AI 资产：
 * - BehaviorTree：真正的行为树图
 * - BlackboardData：行为树使用的键定义
 *
 * 这轮改造前，它靠一个 magic sentinel 自己重新扫项目，
 * 结果 full / incremental / live 主链都不真正经过它。
 *
 * 现在它改成标准 package-scoped indexer：
 * - 直接按真实资产类分发；
 * - artifact / shadow / warmup 都复用同一套 payload；
 * - 不再保留“全局重扫一遍”的死分叉逻辑。
 */
class FBehaviorTreeIndexer : public IMonolithIndexer
{
public:
	/** 只处理真实存在的 BehaviorTree 和 BlackboardData 资产类。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("BehaviorTree"), TEXT("BlackboardData") };
	}

	/** 给日志和调试面板看的名字。 */
	virtual FString GetName() const override { return TEXT("BehaviorTreeIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("BehaviorTree")); }
	/** 构建可缓存的 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 根据运行时对象类型分发到行为树或黑板的 payload 构建逻辑。 */
	bool BuildPayload(UObject* LoadedAsset, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const;
	/** 把行为树对象整理成稳定的图 payload。 */
	bool BuildBehaviorTreePayload(class UBehaviorTree* BehaviorTree, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const;
	/** 把黑板对象整理成“1 个节点 + 多个变量”的 payload。 */
	bool BuildBlackboardPayload(class UBlackboardData* Blackboard, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const;
};
