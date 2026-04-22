#pragma once

#include "MonolithIndexer.h"

/*
 * FGenericAssetIndexer 负责那类“不需要深图遍历，但又值得记元数据”的资源。
 *
 * 例如：
 * - Mesh 的三角面数、材质槽数量
 * - Texture 的尺寸、格式、采样建议
 * - Sound 的时长、采样率、声道数
 *
 * 这类资源通常一条 metadata node 就够用了。
 */
class FGenericAssetIndexer : public IMonolithIndexer
{
public:
	/** 支持一组不需要深图遍历的常见资源。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("StaticMesh"),
			TEXT("SkeletalMesh"),
			TEXT("Texture2D"),
			TEXT("TextureCube"),
			TEXT("SoundWave"),
			TEXT("SoundCue"),
			TEXT("PhysicsAsset"),
			TEXT("Skeleton")
		};
	}

	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("GenericAssetIndexer"); }
	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("GenericAsset")); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
