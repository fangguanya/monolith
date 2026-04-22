#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithArtifactCache.h"
#include "MonolithArtifactTypes.h"
#include "MonolithAssetArtifactPipeline.h"
#include "MonolithIndexDatabase.h"

/*
 * 这组测试专门守住“单资产 artifact 主链”的唯一实现：
 * - cache hit 时必须直接 materialize，不再重复 build；
 * - miss 且允许现场 build 时，必须 build + 写 cache + materialize；
 * - miss 且不允许现场 build 时，必须明确返回 NeedsLocalBuild。
 */

namespace MonolithAssetArtifactPipelineTestInternal
{
	/** 每条测试都使用独立临时数据库，避免互相污染。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithAssetPipeline"), TEXT(".db"));
	}

	/** 构造一份最小可用的 AssetData。 */
	static FAssetData MakeAssetData(
		const FString& PackageName,
		const FString& PackagePath,
		const FString& AssetName,
		const FTopLevelAssetPath& AssetClassPath)
	{
		return FAssetData(
			FName(*PackageName),
			FName(*PackagePath),
			FName(*AssetName),
			AssetClassPath,
			FAssetDataTagMap());
	}

	/** 把短文本 payload 转成 bytes。 */
	static void EncodePayload(const FString& Text, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		FTCHARToUTF8 Convert(*Text);
		if (Convert.Length() > 0)
		{
			OutBytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** 从 bytes 读回测试 payload。 */
	static FString DecodePayload(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0)
		{
			return FString();
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Convert.Length(), Convert.Get());
	}

	/** fake cache 的 key 规则与真实 cache 保持一致。 */
	static FString MakeCacheKey(const FMonolithArtifactIdentityV1& Identity)
	{
		return LexToString(HashMonolithArtifactIdentity(Identity));
	}

	/** fake cache 只保留测试真正关心的 Get / Put 行为。 */
	class FTestArtifactCache final : public IMonolithArtifactCache
	{
	public:
		virtual TOptional<FMonolithArtifact> Get(
			const FMonolithArtifactIdentityV1& Identity,
			const EMonolithArtifactCacheRequestMode RequestMode) override
		{
			(void)RequestMode;
			++GetCallCount;
			if (const FMonolithArtifact* Artifact = StoredArtifacts.Find(MakeCacheKey(Identity)))
			{
				return *Artifact;
			}
			return {};
		}

		virtual bool Put(
			const FMonolithArtifactIdentityV1& Identity,
			const FMonolithArtifact& Artifact,
			const EMonolithArtifactCacheRequestMode RequestMode) override
		{
			(void)RequestMode;
			++PutCallCount;
			StoredArtifacts.Add(MakeCacheKey(Identity), Artifact);
			return true;
		}

		virtual bool DrainRemoteWrites(double TimeoutSeconds = -1.0) override
		{
			(void)TimeoutSeconds;
			return true;
		}

		virtual void DiscardPendingRemoteWrites() override
		{
		}

		virtual FMonolithArtifactCacheStats GetStats() const override
		{
			return FMonolithArtifactCacheStats();
		}

		virtual void ResetStats() override
		{
		}

		virtual void SetIoThreadPool(FQueuedThreadPool* InIoThreadPool) override
		{
			(void)InIoThreadPool;
		}

		void Seed(const FMonolithArtifactIdentityV1& Identity, const FString& PayloadText, const EMonolithExecutionMode ExecutionMode)
		{
			FMonolithArtifact Artifact;
			Artifact.ArtifactSchemaVersion = Identity.ArtifactSchemaVersion;
			Artifact.IndexerId = Identity.IndexerId;
			Artifact.IndexerVersion = Identity.IndexerVersion;
			Artifact.ExecutionMode = ExecutionMode;
			Artifact.PackageName = Identity.PackageName.ToString();
			Artifact.IdentityHash = HashMonolithArtifactIdentity(Identity);
			EncodePayload(PayloadText, Artifact.Payload);
			StoredArtifacts.Add(MakeCacheKey(Identity), Artifact);
		}

		int32 GetCallCount = 0;
		int32 PutCallCount = 0;

	private:
		TMap<FString, FMonolithArtifact> StoredArtifacts;
	};

	/*
	 * fake indexer 只负责观测 pipeline 行为：
	 * - build 时产出固定 payload；
	 * - materialize 时把 payload 写进 DB meta；
	 * - 可选 shadow materialize 也写到另一份 meta。
	 */
	class FTestAssetIndexer final : public IMonolithIndexer
	{
	public:
		FTestAssetIndexer(const FString& InBuildPayloadText, const EMonolithExecutionMode InExecutionMode)
			: BuildPayloadText(InBuildPayloadText)
			, ExecutionMode(InExecutionMode)
		{
		}

		virtual TArray<FString> GetSupportedClasses() const override
		{
			return { TEXT("StaticMesh") };
		}

		virtual FString GetName() const override
		{
			return TEXT("TestAssetIndexer");
		}

		virtual FName GetIndexerId() const override
		{
			return FName(TEXT("TestAsset"));
		}

		virtual uint32 GetIndexerVersion() const override
		{
			return 11;
		}

		virtual uint8 GetArtifactSchemaVersion() const override
		{
			return 5;
		}

		virtual EMonolithExecutionMode GetExecutionMode() const override
		{
			return ExecutionMode;
		}

		virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override
		{
			(void)AssetData;
			(void)LoadedAsset;
			(void)AssetRegistry;
			++BuildArtifactCallCount;
			OutArtifact = FMonolithArtifact();
			EncodePayload(BuildPayloadText, OutArtifact.Payload);
			return true;
		}

		virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override
		{
			(void)AssetId;
			++MaterializeProductionCallCount;
			LastMaterializedPayload = DecodePayload(Artifact.Payload);
			return DB.WriteMeta(TEXT("asset_pipeline_payload"), LastMaterializedPayload);
		}

		virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override
		{
			(void)AssetId;
			(void)CohortName;
			++MaterializeShadowCallCount;
			LastShadowPayload = DecodePayload(Artifact.Payload);
			return DB.WriteMeta(TEXT("asset_pipeline_shadow_payload"), LastShadowPayload);
		}

		int32 BuildArtifactCallCount = 0;
		int32 MaterializeProductionCallCount = 0;
		int32 MaterializeShadowCallCount = 0;
		FString LastMaterializedPayload;
		FString LastShadowPayload;

	private:
		FString BuildPayloadText;
		EMonolithExecutionMode ExecutionMode = EMonolithExecutionMode::AROnly;
	};

	/** 给 fake 资产算出与真实 pipeline 一致的 identity。 */
	static bool BuildTestIdentity(
		const FAssetData& AssetData,
		const IMonolithIndexer& Indexer,
		FMonolithArtifactIdentityV1& OutIdentity)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		return BuildConfiguredMonolithArtifactIdentity(
			AssetData,
			AssetRegistry,
			Indexer.GetIndexerId(),
			Indexer.GetIndexerVersion(),
			Indexer.GetArtifactSchemaVersion(),
			Indexer.GetDependencyVersions(),
			OutIdentity);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerDefaultIndexAssetTest,
	"Monolith.Index.AssetArtifactPipeline.DefaultIndexAssetUsesArtifactPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerDefaultIndexAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DbPath = MonolithAssetArtifactPipelineTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_DefaultIndexAsset");
	Asset.AssetName = TEXT("SM_DefaultIndexAsset");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	const FAssetData AssetData = MonolithAssetArtifactPipelineTestInternal::MakeAssetData(
		Asset.PackagePath,
		TEXT("/Game/Test"),
		Asset.AssetName,
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")));

	// 这里故意直接调用接口默认实现，
	// 用测试守住“包级 IndexAsset 就是 BuildArtifact + MaterializeArtifact”这条唯一主链。
	MonolithAssetArtifactPipelineTestInternal::FTestAssetIndexer Indexer(
		TEXT("default-index-payload"),
		EMonolithExecutionMode::AROnly);

	TestTrue(TEXT("default IndexAsset should succeed through artifact path"), Indexer.IndexAsset(AssetData, nullptr, DB, AssetId));
	TestEqual(TEXT("default IndexAsset should build exactly once"), Indexer.BuildArtifactCallCount, 1);
	TestEqual(TEXT("default IndexAsset should materialize production exactly once"), Indexer.MaterializeProductionCallCount, 1);
	TestEqual(TEXT("default IndexAsset should not touch shadow"), Indexer.MaterializeShadowCallCount, 0);
	TestEqual(TEXT("default IndexAsset should persist built payload"), DB.ReadMeta(TEXT("asset_pipeline_payload")), FString(TEXT("default-index-payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetArtifactPipelineCacheHitTest,
	"Monolith.Index.AssetArtifactPipeline.CacheHitMaterializesProductionAndShadow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetArtifactPipelineCacheHitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DbPath = MonolithAssetArtifactPipelineTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_Pipeline");
	Asset.AssetName = TEXT("SM_Pipeline");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	const FAssetData AssetData = MonolithAssetArtifactPipelineTestInternal::MakeAssetData(
		Asset.PackagePath,
		TEXT("/Game/Test"),
		Asset.AssetName,
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")));

	MonolithAssetArtifactPipelineTestInternal::FTestAssetIndexer Indexer(
		TEXT("built-payload"),
		EMonolithExecutionMode::AROnly);
	MonolithAssetArtifactPipelineTestInternal::FTestArtifactCache Cache;
	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("identity should build"), MonolithAssetArtifactPipelineTestInternal::BuildTestIdentity(AssetData, Indexer, Identity));
	Cache.Seed(Identity, TEXT("cached-payload"), Indexer.GetExecutionMode());

	MonolithAssetArtifactPipeline::FExecuteAssetOptions ExecuteOptions;
	ExecuteOptions.RequestMode = EMonolithArtifactCacheRequestMode::Background;
	ExecuteOptions.bAllowLocalArtifactBuild = false;
	ExecuteOptions.bMaterializeProduction = true;
	ExecuteOptions.AssetId = AssetId;
	ExecuteOptions.ShadowCohortName = TEXT("TestAsset");

	MonolithAssetArtifactPipeline::FExecuteAssetResult ExecuteResult;
	const MonolithAssetArtifactPipeline::EExecuteAssetOutcome Outcome =
		MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
			AssetData,
			nullptr,
			{},
			Indexer,
			&Cache,
			&DB,
			ExecuteOptions,
			ExecuteResult);

	TestEqual(TEXT("cache hit should succeed"), Outcome, MonolithAssetArtifactPipeline::EExecuteAssetOutcome::Succeeded);
	TestTrue(TEXT("cache hit should be reported"), ExecuteResult.bUsedCachedArtifact);
	TestFalse(TEXT("cache hit should skip local build"), ExecuteResult.bBuiltArtifactLocally);
	TestTrue(TEXT("production materialize should run"), ExecuteResult.bMaterializedProduction);
	TestTrue(TEXT("shadow materialize should run"), ExecuteResult.bMaterializedShadow);
	TestEqual(TEXT("build should be skipped on cache hit"), Indexer.BuildArtifactCallCount, 0);
	TestEqual(TEXT("production payload should come from cache"), DB.ReadMeta(TEXT("asset_pipeline_payload")), FString(TEXT("cached-payload")));
	TestEqual(TEXT("shadow payload should come from cache"), DB.ReadMeta(TEXT("asset_pipeline_shadow_payload")), FString(TEXT("cached-payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetArtifactPipelineCacheMissBuildTest,
	"Monolith.Index.AssetArtifactPipeline.CacheMissBuildsStoresAndMaterializes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetArtifactPipelineCacheMissBuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DbPath = MonolithAssetArtifactPipelineTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_PipelineMiss");
	Asset.AssetName = TEXT("SM_PipelineMiss");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	const FAssetData AssetData = MonolithAssetArtifactPipelineTestInternal::MakeAssetData(
		Asset.PackagePath,
		TEXT("/Game/Test"),
		Asset.AssetName,
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")));

	MonolithAssetArtifactPipelineTestInternal::FTestAssetIndexer Indexer(
		TEXT("built-payload"),
		EMonolithExecutionMode::AROnly);
	MonolithAssetArtifactPipelineTestInternal::FTestArtifactCache Cache;

	MonolithAssetArtifactPipeline::FExecuteAssetOptions ExecuteOptions;
	ExecuteOptions.RequestMode = EMonolithArtifactCacheRequestMode::Background;
	ExecuteOptions.bAllowLocalArtifactBuild = true;
	ExecuteOptions.bMaterializeProduction = true;
	ExecuteOptions.AssetId = AssetId;

	MonolithAssetArtifactPipeline::FExecuteAssetResult ExecuteResult;
	const MonolithAssetArtifactPipeline::EExecuteAssetOutcome Outcome =
		MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
			AssetData,
			nullptr,
			{},
			Indexer,
			&Cache,
			&DB,
			ExecuteOptions,
			ExecuteResult);

	TestEqual(TEXT("cache miss with local build should succeed"), Outcome, MonolithAssetArtifactPipeline::EExecuteAssetOutcome::Succeeded);
	TestFalse(TEXT("cache miss should report uncached execution"), ExecuteResult.bUsedCachedArtifact);
	TestTrue(TEXT("cache miss should build locally"), ExecuteResult.bBuiltArtifactLocally);
	TestTrue(TEXT("production materialize should succeed"), ExecuteResult.bMaterializedProduction);
	TestEqual(TEXT("build should run exactly once"), Indexer.BuildArtifactCallCount, 1);
	TestEqual(TEXT("cache should be written exactly once"), Cache.PutCallCount, 1);
	TestEqual(TEXT("materialized payload should come from build result"), DB.ReadMeta(TEXT("asset_pipeline_payload")), FString(TEXT("built-payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetArtifactPipelineNeedsLocalBuildTest,
	"Monolith.Index.AssetArtifactPipeline.CacheMissWithoutBuildPermissionRequestsLocalBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetArtifactPipelineNeedsLocalBuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FAssetData AssetData = MonolithAssetArtifactPipelineTestInternal::MakeAssetData(
		TEXT("/Game/Test/SM_PipelineNeedsWarmup"),
		TEXT("/Game/Test"),
		TEXT("SM_PipelineNeedsWarmup"),
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh")));

	MonolithAssetArtifactPipelineTestInternal::FTestAssetIndexer Indexer(
		TEXT("built-payload"),
		EMonolithExecutionMode::PackageScopedLoad);
	MonolithAssetArtifactPipelineTestInternal::FTestArtifactCache Cache;

	MonolithAssetArtifactPipeline::FExecuteAssetOptions ExecuteOptions;
	ExecuteOptions.RequestMode = EMonolithArtifactCacheRequestMode::Background;
	ExecuteOptions.bAllowLocalArtifactBuild = false;

	MonolithAssetArtifactPipeline::FExecuteAssetResult ExecuteResult;
	const MonolithAssetArtifactPipeline::EExecuteAssetOutcome Outcome =
		MonolithAssetArtifactPipeline::ExecuteAssetIndexerArtifact(
			AssetData,
			nullptr,
			{},
			Indexer,
			&Cache,
			nullptr,
			ExecuteOptions,
			ExecuteResult);

	TestEqual(TEXT("cache miss without build permission should request local build"), Outcome, MonolithAssetArtifactPipeline::EExecuteAssetOutcome::NeedsLocalBuild);
	TestEqual(TEXT("build should not start"), Indexer.BuildArtifactCallCount, 0);
	TestEqual(TEXT("cache should not be written"), Cache.PutCallCount, 0);
	return true;
}
