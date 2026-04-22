#pragma once

#include "MonolithIndexer.h"

namespace MonolithSimpleArtifactSerialization
{
	struct FNodesPayload;
}

/**
 * FNiagaraIndexer 负责把 NiagaraSystem 资产翻译成 Monolith 的 node 摘要。
 *
 * 这轮重构前它是一个 sentinel：
 * - 自己再扫一遍全项目 NiagaraSystem；
 * - 统一在 full index 收尾阶段补写节点。
 *
 * 现在它改成了真正的 package-scoped indexer：
 * - full / incremental / live 都把 Niagara 当普通资产处理；
 * - shadow mode / warmup 也能走统一 artifact 链路；
 * - 不再保留“主链一套、post-pass 再来一套”的重复逻辑。
 */
class FNiagaraIndexer : public IMonolithIndexer
{
public:
	/** 只处理 NiagaraSystem 资产。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("NiagaraSystem") };
	}

	/** 日志和调试里展示的名字。 */
	virtual FString GetName() const override { return TEXT("NiagaraIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Niagara")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 把 NiagaraSystem 整理成稳定的多 node 载荷。 */
	bool BuildPayload(class UNiagaraSystem* System, MonolithSimpleArtifactSerialization::FNodesPayload& OutPayload) const;
	/** 构建系统本体那一条 node。 */
	bool BuildSystemNode(class UNiagaraSystem* System, FIndexedNode& OutNode) const;
	/** 构建单个 emitter 对应的 node。 */
	bool BuildEmitterNode(const struct FNiagaraEmitterHandle& Handle, FIndexedNode& OutNode) const;
};
