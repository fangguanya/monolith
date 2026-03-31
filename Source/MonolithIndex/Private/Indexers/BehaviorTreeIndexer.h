#pragma once

#include "MonolithIndexer.h"

/**
 * 索引行为树资产：UBehaviorTree（节点层级）和 UBlackboardData（键定义）。
 * 采用 Sentinel 模式自行枚举 AssetRegistry，magic class "__BehaviorTrees__"。
 * 包含 BT→Blackboard 和 BT→NodeClass 交叉引用。
 */
class FBehaviorTreeIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("__BehaviorTrees__") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("BehaviorTreeIndexer"); }
	virtual bool IsSentinel() const override { return true; }

	// SEH 安全调用的公共入口
	void IndexBehaviorTreePublic(class UBehaviorTree* BT, FMonolithIndexDatabase& DB, int64 AssetId) { IndexBehaviorTree(BT, DB, AssetId); }
	void IndexBlackboardPublic(class UBlackboardData* BB, FMonolithIndexDatabase& DB, int64 AssetId) { IndexBlackboard(BB, DB, AssetId); }

private:
	void IndexBehaviorTree(class UBehaviorTree* BT, FMonolithIndexDatabase& DB, int64 AssetId);
	void IndexBlackboard(class UBlackboardData* BB, FMonolithIndexDatabase& DB, int64 AssetId);

	/** 序列化 JSON 对象为紧凑字符串 */
	static FString JsonToString(TSharedPtr<FJsonObject> JsonObj);
};
