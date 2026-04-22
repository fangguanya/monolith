#pragma once

#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "MonolithIndexer.h"

/*
 * 这个 indexer 只负责 GameplayTag 的“全局定义树”。
 *
 * 之所以把它从 GameplayTagIndexer 里单独拆出来，是为了把两种完全不同的职责分开：
 * - `GameplayTagIndexer`：单资产 companion，关注“谁引用了哪些 tag”；
 * - `GameplayTagDefinitionIndexer`：全局 reducer，关注“项目里定义了哪些 tag”。
 *
 * 这样执行模式、warmup scope、artifact identity 都能保持单一语义。
 */
class FGameplayTagDefinitionIndexer : public IMonolithIndexer
{
public:
	/** 全局 reducer 不通过资产类分发。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {};
	}

	/** 真正的全局定义刷新入口。 */
	virtual bool IndexGlobal(FMonolithIndexDatabase& DB) override;
	/** 为 tag 定义树构建全局 identity。 */
	virtual bool BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const override;
	/** 为 tag 定义树构建 artifact。 */
	virtual bool BuildGlobalArtifact(FMonolithArtifact& OutArtifact) override;
	/** 把 tag 定义 artifact 回放到正式 SQLite。 */
	virtual bool MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB) override;
	virtual FString GetName() const override { return TEXT("GameplayTagDefinitionIndexer"); }
	/** 用单独稳定 id，把“定义树”和“资产引用”彻底分开。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("GameplayTagDefinitions")); }
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::GlobalReducer; }

private:
	/** 读取当前 tag 管理器里的定义树，并收集成统一 payload。 */
	static bool BuildPayload(MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& OutPayload);
	/** 递归把一棵 tag 子树展开成平铺列表。 */
	static void AppendNodeDefinitions(
		const struct FGameplayTagNode& Node,
		MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& OutPayload);
};
