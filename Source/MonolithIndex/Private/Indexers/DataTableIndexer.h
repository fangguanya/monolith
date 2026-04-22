#pragma once

#include "MonolithIndexer.h"

/*
 * FDataTableIndexer 负责把整张 DataTable 拆成“很多行记录”。
 *
 * 和别的单 node indexer 不同，
 * DataTable 最重要的内容就是每一行本身，所以这里的核心产物是 rows。
 * 这也正好适合 revision / promote / rollback 的逐行聚合比较。
 */
class FDataTableIndexer : public IMonolithIndexer
{
public:
	/** 只接管 DataTable。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("DataTable") };
	}

	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("DataTable")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("DataTableIndexer"); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 把整张表转成行数组载荷。 */
	bool BuildPayload(class UDataTable* DataTable, TArray<FIndexedDataTableRow>& OutRows) const;
	/** 把单行结构体转成 JSON 文本。 */
	static FString RowStructToJson(const UScriptStruct* RowStruct, const void* RowData);
};
