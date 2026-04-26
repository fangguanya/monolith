#pragma once

#include "MonolithIndexer.h"

/*
 * FAssetVisualGeometricIndexer：AssetVisual cohort 的 geometric provider 一边。
 *
 * 它和 FMeshCatalogIndexer 一样是 companion indexer：
 *  - 不参与 GenericAsset 主 class dispatch；
 *  - 显式 `Scope=Cohort:AssetVisualGeometric` warmup 时才被命中；
 *  - 只对 LoadedAsset != nullptr 的 4 类资产生效，类型分发由 IAssetCanonicalRenderer 内部完成。
 *
 * 支持的 4 类资产：
 *  - StaticMesh             → mesh component + bounds-fit 相机
 *  - SkeletalMesh           → skel mesh component + ref pose bounds
 *  - Material / MIC         → 把 material 贴到固定球体上 + 固定相机
 *  - WidgetBlueprint        → SWidget → Texture，rasterize 到 256
 *
 * BuildArtifact 的执行时机：
 *  - 必须在游戏线程被调用（依赖 GEditor / 渲染线程）；
 *  - artifact identity 含 ProviderId / ProviderVersion / RenderRecipeVersion；
 *  - 任意一个变化都会让 artifact 失效，强制重新渲染 + 重算 embedding。
 */
class FAssetVisualGeometricIndexer : public IMonolithIndexer
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

	virtual FString GetName() const override { return TEXT("AssetVisualGeometricIndexer"); }
	virtual FName GetIndexerId() const override { return FName(TEXT("AssetVisualGeometric")); }
	virtual uint32 GetIndexerVersion() const override { return 1; }
	virtual uint8 GetArtifactSchemaVersion() const override { return 1; }
	/** 视觉索引必须由离线 warmup 触发，不参与 live / incremental 同步路径。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::OfflineOnly; }

	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
