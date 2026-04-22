#pragma once

#include "MonolithIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"

/*
 * FDataAssetIndexer 负责把“主要靠属性配置”的 DataAsset 翻译成 Monolith 能理解的结构。
 *
 * 它会同时产出：
 * - 一个 node：保存整棵属性树的大 JSON；
 * - 一组 variables：把适合单独搜索和 diff 的属性拆出来。
 *
 * 这样查询侧既能看全貌，也能按字段搜索，还能把结果缓存成 artifact。
 */
class FDataAssetIndexer : public IMonolithIndexer
{
public:
	/** 这个 indexer 接管常见 DataAsset 基类。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("PrimaryDataAsset"),
			TEXT("DataAsset")
		};
	}

	/** shadow / warmup / artifact 身份里使用的稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("DataAsset")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("DataAssetIndexer"); }
	/** 构建可缓存的 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 统一构建 DataAsset 的 node+variables 载荷。 */
	static bool BuildPayload(UObject* Object, MonolithSimpleArtifactSerialization::FNodeVariablePayload& OutPayload);
	/** 这些属性不会进 node 的 JSON 大快照。 */
	static bool ShouldSkipNodeProperty(const FProperty* Prop);
	/** 这些属性不会被拆成单独 variable。 */
	static bool ShouldSkipVariableProperty(const FProperty* Prop);
	/** 把整个 UObject 的属性树序列化成 JSON 对象。 */
	static TSharedPtr<FJsonObject> SerializeObjectProperties(UObject* Object);
	/** 把单个 UPROPERTY 值转换成 JSON 值。 */
	static TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, const UObject* Owner);
};
