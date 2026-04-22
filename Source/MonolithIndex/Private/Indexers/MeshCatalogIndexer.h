#pragma once

#include "MonolithIndexer.h"

/*
 * FMeshCatalogIndexer 负责给 StaticMesh 生成一条可做尺寸检索的目录行。
 *
 * 它现在不再做“全项目重扫”的 post-pass，
 * 而是改成跟随每个静态网格资产一起更新：
 * - full / incremental / live 都按单资产写 revision；
 * - shadow mode 可以直接比较单资产目录行；
 * - warmup / artifact cache 也共用同一套序列化逻辑。
 *
 * 这条链路之所以单独保留成一个 indexer，
 * 是因为 mesh catalog 的查询需求和 GenericAsset 的 metadata node 不一样：
 * - GenericAsset 更偏“给人读”的摘要；
 * - MeshCatalog 更偏“给范围查询用”的结构化数值列。
 */
class FMeshCatalogIndexer : public IMonolithIndexer
{
public:
	/** Mesh catalog 只处理 StaticMesh。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("StaticMesh") };
	}

	/** 日志和调试时展示的名字。 */
	virtual FString GetName() const override { return TEXT("MeshCatalogIndexer"); }
	/** shadow / artifact / metadata 使用的稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("MeshCatalog")); }
	/** 构建 mesh catalog artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 把 artifact 回放到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 把 artifact 回放到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 从 StaticMesh 提取可范围查询的目录字段。 */
	bool BuildPayload(const FAssetData& AssetData, class UStaticMesh* Mesh, FIndexedMeshCatalogEntry& OutEntry) const;
};
