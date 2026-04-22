#pragma once

#include "MonolithIndexer.h"

/*
 * FUserDefinedEnumIndexer 负责用户自定义枚举。
 *
 * 它会产出：
 * - 一个 node：记录枚举整体摘要；
 * - 一组 variables：每个枚举项一条，方便搜索和 shadow diff。
 */
class FUserDefinedEnumIndexer : public IMonolithIndexer
{
public:
	/** 只接管用户自定义枚举。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("UserDefinedEnum") };
	}

	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("UserDefinedEnum")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("UserDefinedEnumIndexer"); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
