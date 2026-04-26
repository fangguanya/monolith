#pragma once

#include "MonolithIndexer.h"

/*
 * FAssetVisualSemanticIndexer：AssetVisual cohort 的 CLIP semantic provider 一边。
 *
 * 支持类与 geometric 完全对齐（StaticMesh / SkeletalMesh / Material / MIC / WidgetBlueprint），
 * canonical iso 渲染由 IAssetCanonicalRenderer 内部按运行时类型分发。
 *
 * 与 geometric indexer 的差异：
 *  - 只渲染 iso 一张视图（CLIP 自带视角不变性）
 *  - 不计算 silhouette
 *  - 只对 IsAvailable() 为 true 的 provider 生效（NNE 不可用 / 模型缺失时整 cohort 跳过）
 *  - 共享 geometric indexer 写下的 iso PNG（不重复持久化）
 *
 * provider 不可用是一等支持状态，spec 要求：
 *  - asset 标记为 stale
 *  - 不阻塞 MeshCatalog / AssetVisualGeometric 的同资产索引
 */
class FAssetVisualSemanticIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("StaticMesh"),
			TEXT("SkeletalMesh"),
			TEXT("Material"),
			TEXT("MaterialInstanceConstant"),
			TEXT("WidgetBlueprint"),
		};
	}

	virtual FString GetName() const override { return TEXT("AssetVisualSemanticIndexer"); }
	virtual FName GetIndexerId() const override { return FName(TEXT("AssetVisualSemantic")); }
	virtual uint32 GetIndexerVersion() const override { return 1; }
	virtual uint8 GetArtifactSchemaVersion() const override { return 1; }
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::OfflineOnly; }

	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
