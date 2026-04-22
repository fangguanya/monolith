#include "Indexers/DependencyIndexer.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"

/*
 * DependencyIndexer 的核心目标是“只维护一份 dependency 真相”：
 * - 生产路径直接把 payload 回放到正式表；
 * - artifact 路径把同一份 payload 写进缓存；
 * - shadow 路径把同一份 payload 回放到影子表。
 *
 * 这样就不会出现“正式索引逻辑一套，artifact 逻辑又偷偷是另一套”的重复实现。
 */

namespace DependencyIndexerInternal
{
	/** 直接从 Asset Registry 读取某个包的依赖关系，并转成稳定 payload。 */
	static bool BuildPayload(
		const FAssetData& AssetData,
		IAssetRegistry& Registry,
		MonolithSimpleArtifactSerialization::FDependencyPayload& OutPayload)
	{
		OutPayload = MonolithSimpleArtifactSerialization::FDependencyPayload();
		if (AssetData.PackageName.IsNone())
		{
			return false;
		}

		auto AppendDependencies = [&Registry, &AssetData, &OutPayload](
			const UE::AssetRegistry::EDependencyQuery Query,
			const TCHAR* DependencyType)
		{
			TArray<FAssetIdentifier> Dependencies;
			Registry.GetDependencies(
				AssetData.PackageName,
				Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package,
				Query);

			for (const FAssetIdentifier& DependencyIdentifier : Dependencies)
			{
				if (DependencyIdentifier.PackageName.IsNone())
				{
					continue;
				}

				// artifact 里只保留真正稳定的语义字段：
				// 目标包路径 + 依赖类型。
				MonolithSimpleArtifactSerialization::FDependencyPayloadEntry Entry;
				Entry.TargetPackagePath = DependencyIdentifier.PackageName.ToString();
				Entry.DependencyType = DependencyType;
				OutPayload.Dependencies.Add(MoveTemp(Entry));
			}
		};

		AppendDependencies(UE::AssetRegistry::EDependencyQuery::Hard, TEXT("Hard"));
		AppendDependencies(UE::AssetRegistry::EDependencyQuery::Soft, TEXT("Soft"));

		OutPayload.Dependencies.Sort([](
			const MonolithSimpleArtifactSerialization::FDependencyPayloadEntry& A,
			const MonolithSimpleArtifactSerialization::FDependencyPayloadEntry& B)
		{
			if (A.TargetPackagePath != B.TargetPackagePath)
			{
				return A.TargetPackagePath < B.TargetPackagePath;
			}

			return A.DependencyType < B.DependencyType;
		});

		return true;
	}
}

bool FDependencyIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)LoadedAsset;

	MonolithSimpleArtifactSerialization::FDependencyPayload Payload;
	if (!DependencyIndexerInternal::BuildPayload(AssetData, AssetRegistry, Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeDependencyPayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FDependencyIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FDependencyPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeDependencyPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeDependencyPayload(Payload, DB, AssetId);
}

bool FDependencyIndexer::MaterializeArtifactToShadow(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	int64 AssetId,
	const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FDependencyPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeDependencyPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeDependencyPayloadToShadow(Payload, DB, AssetId, CohortName);
}
