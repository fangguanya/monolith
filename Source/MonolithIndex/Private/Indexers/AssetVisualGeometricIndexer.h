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
			TEXT("NiagaraSystem"),
			TEXT("NiagaraEmitter"),
			TEXT("AnimSequence"),
			TEXT("AnimMontage"),
			TEXT("AnimBlueprint"),
		};
	}

	virtual FString GetName() const override { return TEXT("AssetVisualGeometricIndexer"); }
	virtual FName GetIndexerId() const override { return FName(TEXT("AssetVisualGeometric")); }
	// v2: SCS_BaseColor（黑）；v3: unlit show flag（仍黑）；v4: magenta diag —— 验证 scene_proxy=nil
	// + ClearColor 都没生效，根因是 EditorWorld 在 commandlet 下不能挂 SCC2D。
	// v5: FPreviewScene（pixel 仍 0，scene_proxy 仍 nil，因为 SendRenderState 没 flush）；
	// v14: 扩展 supported classes 加入 NiagaraSystem / NiagaraEmitter / AnimSequence / AnimMontage / AnimBlueprint。
	// 这些资产 UE 都有现成的 thumbnail renderer，UThumbnailManager::GetRenderingInfo 自动分发。
	// v15: AssetVisualArtifact payload schema v1→v2（多 phase 数组化），entry 加 phase_id/phase_t/phase_label。
	// 必须 bump，否则 DDC 缓存里残留的 v1 字节流会以同 identity 命中 → MaterializeArtifact deserialize 失败 → 永远不重 build。
	// v16: BuildArtifact 改为按 GetPhasesForAsset 循环渲染 N phase + 每 phase 独立 PNG（payload v2→v3），
	//      RenderRecipeVersion 4→5 接入 anim/niagara 旁路。Identity 含 IndexerVersion 不含 RenderRecipeVersion，
	//      所以必须 bump 让 DDC 旧 v15 单 phase 字节流彻底失效。
	virtual uint32 GetIndexerVersion() const override { return 16; }
	virtual uint8 GetArtifactSchemaVersion() const override { return 1; }
	/** 视觉索引必须由离线 warmup 触发，不参与 live / incremental 同步路径。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::OfflineOnly; }

	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;
};
