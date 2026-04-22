#include "MonolithAssetArtifactPipeline.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexer.h"
#include "MonolithIndexDatabase.h"

namespace MonolithAssetArtifactPipelineInternal
{
	/** 把本地新建 artifact 的稳定元数据补齐到统一格式。
	 *
	 * 这样不管 artifact 是由 warmup 还是编辑器生产，
	 * 外部看到的字段布局都保持一致。 */
	static void FinalizeBuiltArtifact(
		const FAssetData& AssetData,
		const IMonolithIndexer& Indexer,
		const FMonolithArtifactIdentityV1& Identity,
		FMonolithArtifact& InOutArtifact)
	{
		InOutArtifact.ArtifactSchemaVersion = Indexer.GetArtifactSchemaVersion();
		InOutArtifact.IndexerId = Indexer.GetIndexerId();
		InOutArtifact.IndexerVersion = Indexer.GetIndexerVersion();
		InOutArtifact.ExecutionMode = Indexer.GetExecutionMode();
		InOutArtifact.PackageName = AssetData.PackageName.ToString();
		InOutArtifact.IdentityHash = HashMonolithArtifactIdentity(Identity);
	}
}

namespace MonolithAssetArtifactPipeline
{
	EExecuteAssetOutcome ExecuteAssetIndexerArtifact(
		const FAssetData& AssetData,
		UObject* LoadedAsset,
		TUniqueFunction<UObject*()> LoadAssetForLocalBuild,
		IMonolithIndexer& Indexer,
		IMonolithArtifactCache* ArtifactCache,
		FMonolithIndexDatabase* DB,
		const FExecuteAssetOptions& Options,
		FExecuteAssetResult& OutResult)
	{
		OutResult = FExecuteAssetResult();

		if (!ArtifactCache)
		{
			return EExecuteAssetOutcome::Failed;
		}

		const bool bNeedsDatabaseWrite = Options.bMaterializeProduction || !Options.ShadowCohortName.IsEmpty();
		if (bNeedsDatabaseWrite && (!DB || Options.AssetId <= 0))
		{
			return EExecuteAssetOutcome::Failed;
		}

		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		FMonolithArtifactIdentityV1 Identity;
		if (!BuildConfiguredMonolithArtifactIdentity(
			AssetData,
			AssetRegistry,
			Indexer.GetIndexerId(),
			Indexer.GetIndexerVersion(),
			Indexer.GetArtifactSchemaVersion(),
			Indexer.GetDependencyVersions(),
			Identity))
		{
			return EExecuteAssetOutcome::Failed;
		}
		OutResult.Identity = Identity;

		FMonolithArtifact Artifact;
		if (const TOptional<FMonolithArtifact> CachedArtifact = ArtifactCache->Get(Identity, Options.RequestMode))
		{
			Artifact = CachedArtifact.GetValue();
			OutResult.bUsedCachedArtifact = true;
		}
		else
		{
			if (!Options.bAllowLocalArtifactBuild)
			{
				return EExecuteAssetOutcome::NeedsLocalBuild;
			}

			UObject* LocalLoadedAsset = LoadedAsset;
			if (!LocalLoadedAsset && Indexer.GetExecutionMode() != EMonolithExecutionMode::AROnly)
			{
				if (!LoadAssetForLocalBuild)
				{
					return EExecuteAssetOutcome::Failed;
				}

				LocalLoadedAsset = LoadAssetForLocalBuild();
				if (!LocalLoadedAsset)
				{
					return EExecuteAssetOutcome::Failed;
				}
			}

			if (!Indexer.BuildArtifact(AssetData, LocalLoadedAsset, AssetRegistry, Artifact))
			{
				return EExecuteAssetOutcome::Failed;
			}

			MonolithAssetArtifactPipelineInternal::FinalizeBuiltArtifact(AssetData, Indexer, Identity, Artifact);
			if (!ArtifactCache->Put(Identity, Artifact, Options.RequestMode))
			{
				return EExecuteAssetOutcome::Failed;
			}

			OutResult.bBuiltArtifactLocally = true;
		}

		if (Options.bMaterializeProduction)
		{
			if (!Indexer.MaterializeArtifact(Artifact, *DB, Options.AssetId))
			{
				return EExecuteAssetOutcome::Failed;
			}

			OutResult.bMaterializedProduction = true;
		}

		if (!Options.ShadowCohortName.IsEmpty())
		{
			OutResult.bMaterializedShadow =
				Indexer.MaterializeArtifactToShadow(Artifact, *DB, Options.AssetId, Options.ShadowCohortName);
		}

		return EExecuteAssetOutcome::Succeeded;
	}
}
