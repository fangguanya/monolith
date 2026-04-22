#pragma once

#include "MonolithIndexer.h"

/*
 * FBlueprintIndexer 是最“图结构味”最重的 indexer 之一。
 *
 * 它会把 Blueprint 里的：
 * - 节点
 * - 引脚连线
 * - 变量声明
 * 都提出来，写进 Monolith 的统一图模型。
 *
 * 这也是 shadow/artifact 链路里最典型的一条样板实现。
 */
class FBlueprintIndexer : public IMonolithIndexer
{
public:
	/** 支持普通 Blueprint、WidgetBlueprint 和 AnimBlueprint。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("Blueprint"), TEXT("WidgetBlueprint"), TEXT("AnimBlueprint") };
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("BlueprintIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Blueprint")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
