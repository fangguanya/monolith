#pragma once

#include "MonolithIndexer.h"

/*
 * FLevelIndexer 负责把关卡里的 Actor 列表抽出来。
 *
 * 现在它不再走旧的 sentinel 模式，
 * 而是把 World 当成一个正常资产来处理：
 * - BuildArtifact
 * - MaterializeArtifact
 * - MaterializeArtifactToShadow
 *
 * 这样 Level cohort 也能吃到 revision / promote / rollback / shadow diff 的完整链路。
 */
class FLevelIndexer : public IMonolithIndexer
{
public:
	/** Level 资产在 AR 里真正的类名是 World。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("World") };
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("LevelIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Level")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow actors 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 从已加载世界里抽出 actor 列表。 */
	bool BuildActorPayload(UObject* LoadedAsset, TArray<FIndexedActor>& OutActors) const;
	/** 从某个具体 ULevel 里提取 actor。 */
	bool BuildActorsInLevel(class ULevel* Level, TArray<FIndexedActor>& OutActors) const;
	/** 把 actor 列表写回正式表。 */
	bool MaterializeActors(const TArray<FIndexedActor>& Actors, FMonolithIndexDatabase& DB, int64 AssetId) const;
	/** 把 actor 列表写回 shadow 表。 */
	bool MaterializeActorsToShadow(const TArray<FIndexedActor>& Actors, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) const;
	/** 序列化 actor 载荷。 */
	static void SerializePayload(const TArray<FIndexedActor>& Actors, TArray<uint8>& OutBytes);
	/** 反序列化 actor 载荷。 */
	static bool DeserializePayload(const TArray<uint8>& Bytes, TArray<FIndexedActor>& OutActors);
	/** 把 Transform 压成 JSON 文本。 */
	FString SerializeTransform(const FTransform& Transform) const;
	/** 把组件摘要压成 JSON 文本。 */
	FString SerializeComponents(const class AActor* Actor) const;
};
