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
	// v2: SCS_BaseColor（黑）；v3: unlit show flag（仍黑）；v4: magenta diag —— 验证 scene_proxy=nil
	// + ClearColor 都没生效，根因是 EditorWorld 在 commandlet 下不能挂 SCC2D。
	// v5: FPreviewScene（pixel 仍 0，scene_proxy 仍 nil，因为 SendRenderState 没 flush）；
	// v13: 放弃 commandlet 渲染——已确认 UE 5.7 commandlet 模式无法渲染 mesh/material（无论 SCC2D /
	// PreviewScene / UThumbnailManager 哪条路）。warmup 必须在编辑器进程里跑（Slate + GUnrealEd 全到位）。
	// MaterializeAssetVisual console command 同时承担 build + materialize（bAllowLocalArtifactBuild=true）。
	virtual uint32 GetIndexerVersion() const override { return 13; }
	virtual uint8 GetArtifactSchemaVersion() const override { return 1; }
	/** 视觉索引必须由离线 warmup 触发，不参与 live / incremental 同步路径。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::OfflineOnly; }

	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
