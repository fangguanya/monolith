#pragma once

#include "MonolithIndexer.h"

/*
 * FMaterialIndexer 负责材质体系的索引：
 * - 材质节点
 * - 节点连线
 * - 参数摘要
 *
 * 它和 BlueprintIndexer 很像，都是“图 + 连接 + 附加数据”的模式，
 * 只是数据来源从蓝图图编辑器换成了材质表达式图。
 */
class FMaterialIndexer : public IMonolithIndexer
{
public:
	/** 支持 Material、MIC 和 MaterialFunction。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("Material"),
			TEXT("MaterialInstanceConstant"),
			TEXT("MaterialFunction")
		};
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("MaterialIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Material")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
};
