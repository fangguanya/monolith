#pragma once

#include "MonolithIndexer.h"

/**
 * 索引 EQS（Environment Query System）资产：UEnvQuery。
 * 提取每个 Option 的 Generator 和 Tests 信息，建立类交叉引用。
 * 采用 Sentinel 模式，magic class "__EQS__"。
 */
class FEQSIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__EQS__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("EQSIndexer"); }
	virtual bool IsSentinel() const override { return true; }

	// SEH 安全调用的公共入口
	void IndexEnvQueryPublic(class UEnvQuery* Query, FMonolithIndexDatabase& DB, int64 AssetId) { IndexEnvQuery(Query, DB, AssetId); }

private:
	void IndexEnvQuery(class UEnvQuery* Query, FMonolithIndexDatabase& DB, int64 AssetId);

	static FString JsonToString(TSharedPtr<FJsonObject> JsonObj);
};
