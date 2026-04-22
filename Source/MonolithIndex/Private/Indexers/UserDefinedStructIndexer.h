#pragma once

#include "MonolithIndexer.h"

/*
 * FUserDefinedStructIndexer 负责用户自定义结构体。
 *
 * 它会把：
 * - 结构体整体字段列表写进 node；
 * - 每个字段拆成 variable。
 *
 * 这样查询既能看到“这个 Struct 有哪些字段”，
 * 也能直接搜字段名和默认值。
 */
class FUserDefinedStructIndexer : public IMonolithIndexer
{
public:
	/** 只接管用户自定义结构体。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("UserDefinedStruct") };
	}

	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("UserDefinedStruct")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("UserDefinedStructIndexer"); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
