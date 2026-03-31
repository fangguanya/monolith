#pragma once

// StateTree 仅在引擎包含 StateTreeModule 时可用
#if WITH_STATETREE

#include "MonolithIndexer.h"

/**
 * 索引 StateTree 资产：UStateTree。
 * 提取每个 State 和 Task 信息，建立状态转换和任务类交叉引用。
 * 采用 Sentinel 模式，magic class "__StateTrees__"。
 * 条件编译：仅在 WITH_STATETREE 定义时启用。
 */
class FStateTreeIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__StateTrees__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("StateTreeIndexer"); }
	virtual bool IsSentinel() const override { return true; }

	// SEH 安全调用的公共入口
	void IndexStateTreePublic(class UStateTree* ST, FMonolithIndexDatabase& DB, int64 AssetId) { IndexStateTree(ST, DB, AssetId); }

private:
	void IndexStateTree(class UStateTree* ST, FMonolithIndexDatabase& DB, int64 AssetId);

	static FString JsonToString(TSharedPtr<FJsonObject> JsonObj);
};

#endif // WITH_STATETREE
