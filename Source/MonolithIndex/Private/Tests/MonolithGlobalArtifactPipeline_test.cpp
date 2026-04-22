#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Indexers/MonolithGlobalArtifactHelpers.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithArtifactCache.h"
#include "MonolithGlobalArtifactPipeline.h"
#include "MonolithIndexDatabase.h"

/*
 * 这组测试专门守住 GlobalReducer artifact 主链：
 * - warmup 命中 cache 时，不能再重复 build；
 * - execute 命中 cache 时，必须直接 materialize cached artifact；
 * - miss 时要 build + materialize + 回写 cache，一步都不能少。
 */

namespace MonolithGlobalArtifactPipelineTestInternal
{
	/** 每个测试都使用独立临时数据库，避免互相污染。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithGlobalPipeline"), TEXT(".db"));
	}

	/** 把测试里用的短字符串 payload 转成 artifact bytes。 */
	static void EncodePayload(const FString& Text, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		FTCHARToUTF8 Convert(*Text);
		if (Convert.Length() > 0)
		{
			OutBytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** 从 artifact bytes 里读回测试字符串。 */
	static FString DecodePayload(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0)
		{
			return FString();
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Convert.Length(), Convert.Get());
	}

	/** 把 identity hash 转成 map key，保证 fake cache 的 key 规则与真实 cache 一致。 */
	static FString MakeCacheKey(const FMonolithArtifactIdentityV1& Identity)
	{
		return LexToString(HashMonolithArtifactIdentity(Identity));
	}

	/*
	 * fake cache 只实现这组测试真正关心的行为：
	 * - Get / Put 是否被调用；
	 * - 最终缓存里放进去的是哪份 artifact。
	 */
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

		void Seed(const FMonolithArtifactIdentityV1& Identity, const FString& PayloadText)
		{
			FMonolithArtifact Artifact;
			Artifact.IndexerId = Identity.IndexerId;
			Artifact.IndexerVersion = Identity.IndexerVersion;
			Artifact.ArtifactSchemaVersion = Identity.ArtifactSchemaVersion;
			Artifact.ExecutionMode = EMonolithExecutionMode::GlobalReducer;
			Artifact.PackageName = Identity.PackageName.ToString();
			Artifact.IdentityHash = HashMonolithArtifactIdentity(Identity);
			EncodePayload(PayloadText, Artifact.Payload);
			StoredArtifacts.Add(MakeCacheKey(Identity), Artifact);
		}

		TOptional<FMonolithArtifact> Find(const FMonolithArtifactIdentityV1& Identity) const
		{
			if (const FMonolithArtifact* Artifact = StoredArtifacts.Find(MakeCacheKey(Identity)))
			{
				return *Artifact;
			}
			return {};
		}

		int32 GetCallCount = 0;
		int32 PutCallCount = 0;

	private:
		TMap<FString, FMonolithArtifact> StoredArtifacts;
	};

	/*
	 * fake indexer 用来观测 pipeline 行为，而不是测试具体业务 indexer。
	 *
	 * 它会把 materialize 结果写到 DB meta，
	 * 这样我们就能精确判断“最后落盘的到底是 build 出来的 artifact，还是 cache 命中的 artifact”。
	 */
	class FTestGlobalIndexer final : public IMonolithIndexer
	{
	public:
		explicit FTestGlobalIndexer(const FString& InBuildPayloadText)
			: BuildPayloadText(InBuildPayloadText)
		{
		}

		virtual TArray<FString> GetSupportedClasses() const override
		{
			return {};
		}

		virtual FString GetName() const override
		{
			return TEXT("TestGlobalIndexer");
		}

		virtual FName GetIndexerId() const override
		{
			return FName(TEXT("TestGlobal"));
		}

		virtual uint32 GetIndexerVersion() const override
		{
			return 7;
		}

		virtual uint8 GetArtifactSchemaVersion() const override
		{
			return 3;
		}

		virtual EMonolithExecutionMode GetExecutionMode() const override
		{
			return EMonolithExecutionMode::GlobalReducer;
		}

		virtual bool BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const override
		{
			++BuildIdentityCallCount;
			MonolithGlobalArtifactHelpers::BuildGlobalManifestIdentity(
				*this,
				FName(TEXT("/Monolith/Test/GlobalReducer")),
				Fingerprint,
				OutIdentity);
			return true;
		}

		virtual bool BuildGlobalArtifact(FMonolithArtifact& OutArtifact) override
		{
			++BuildArtifactCallCount;
			OutArtifact = FMonolithArtifact();
			OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
			OutArtifact.IndexerId = GetIndexerId();
			OutArtifact.IndexerVersion = GetIndexerVersion();
			OutArtifact.ExecutionMode = GetExecutionMode();
			OutArtifact.PackageName = TEXT("/Monolith/Test/GlobalReducer");
			EncodePayload(BuildPayloadText, OutArtifact.Payload);
			return true;
		}

		virtual bool MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB) override
		{
			++MaterializeCallCount;
			LastMaterializedPayload = DecodePayload(Artifact.Payload);
			return DB.WriteMeta(TEXT("test_global_pipeline_payload"), LastMaterializedPayload);
		}

		FString BuildPayloadText;
		FString Fingerprint = TEXT("pipeline-fingerprint");
		mutable int32 BuildIdentityCallCount = 0;
		int32 BuildArtifactCallCount = 0;
		int32 MaterializeCallCount = 0;
		FString LastMaterializedPayload;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGlobalArtifactWarmupCacheHitTest,
	"Monolith.Index.GlobalArtifactPipeline.WarmupUsesCacheHitWithoutRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGlobalArtifactWarmupCacheHitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	MonolithGlobalArtifactPipelineTestInternal::FTestGlobalIndexer Indexer(TEXT("build-payload"));
	MonolithGlobalArtifactPipelineTestInternal::FTestArtifactCache Cache;

	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("global identity should build"), Indexer.BuildGlobalArtifactIdentity(Identity));
	Cache.Seed(Identity, TEXT("cached-payload"));
	Indexer.BuildIdentityCallCount = 0;

	TestTrue(TEXT("warmup should treat cache hit as success"), MonolithGlobalArtifactPipeline::WarmGlobalIndexerArtifact(Indexer, Cache));
	TestEqual(TEXT("cache hit should skip global artifact rebuild"), Indexer.BuildArtifactCallCount, 0);
	TestEqual(TEXT("warmup should not rewrite cache on hit"), Cache.PutCallCount, 0);
	TestEqual(TEXT("warmup should still query cache once"), Cache.GetCallCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGlobalArtifactWarmupCacheMissTest,
	"Monolith.Index.GlobalArtifactPipeline.WarmupBuildsAndStoresOnMiss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGlobalArtifactWarmupCacheMissTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	MonolithGlobalArtifactPipelineTestInternal::FTestGlobalIndexer Indexer(TEXT("build-payload"));
	MonolithGlobalArtifactPipelineTestInternal::FTestArtifactCache Cache;

	TestTrue(TEXT("warmup should build and cache artifact on miss"), MonolithGlobalArtifactPipeline::WarmGlobalIndexerArtifact(Indexer, Cache));
	TestEqual(TEXT("cache miss should build artifact exactly once"), Indexer.BuildArtifactCallCount, 1);
	TestEqual(TEXT("cache miss should write artifact back once"), Cache.PutCallCount, 1);

	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("identity should still be reproducible after warmup"), Indexer.BuildGlobalArtifactIdentity(Identity));
	const TOptional<FMonolithArtifact> CachedArtifact = Cache.Find(Identity);
	TestTrue(TEXT("artifact should be present in fake cache after warmup"), CachedArtifact.IsSet());
	TestEqual(
		TEXT("cached artifact payload should be the freshly built one"),
		CachedArtifact.IsSet() ? MonolithGlobalArtifactPipelineTestInternal::DecodePayload(CachedArtifact->Payload) : FString(),
		FString(TEXT("build-payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGlobalArtifactExecuteCacheHitTest,
	"Monolith.Index.GlobalArtifactPipeline.ExecuteUsesCachedArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGlobalArtifactExecuteCacheHitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DbPath = MonolithGlobalArtifactPipelineTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	MonolithGlobalArtifactPipelineTestInternal::FTestGlobalIndexer Indexer(TEXT("build-payload"));
	MonolithGlobalArtifactPipelineTestInternal::FTestArtifactCache Cache;
	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("global identity should build"), Indexer.BuildGlobalArtifactIdentity(Identity));
	Cache.Seed(Identity, TEXT("cached-payload"));
	Indexer.BuildIdentityCallCount = 0;

	bool bUsedCachedArtifact = false;
	TestTrue(
		TEXT("execute should succeed from cached artifact"),
		MonolithGlobalArtifactPipeline::ExecuteGlobalIndexerArtifact(Indexer, &Cache, DB, bUsedCachedArtifact));
	TestTrue(TEXT("execute should report cached artifact usage"), bUsedCachedArtifact);
	TestEqual(TEXT("cache hit should skip artifact rebuild"), Indexer.BuildArtifactCallCount, 0);
	TestEqual(TEXT("execute should materialize exactly once"), Indexer.MaterializeCallCount, 1);
	TestEqual(TEXT("database should contain cached payload"), DB.ReadMeta(TEXT("test_global_pipeline_payload")), FString(TEXT("cached-payload")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGlobalArtifactExecuteCacheMissTest,
	"Monolith.Index.GlobalArtifactPipeline.ExecuteBuildsMaterializesAndStoresOnMiss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGlobalArtifactExecuteCacheMissTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DbPath = MonolithGlobalArtifactPipelineTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	MonolithGlobalArtifactPipelineTestInternal::FTestGlobalIndexer Indexer(TEXT("build-payload"));
	MonolithGlobalArtifactPipelineTestInternal::FTestArtifactCache Cache;

	bool bUsedCachedArtifact = true;
	TestTrue(
		TEXT("execute should build and materialize on cache miss"),
		MonolithGlobalArtifactPipeline::ExecuteGlobalIndexerArtifact(Indexer, &Cache, DB, bUsedCachedArtifact));
	TestFalse(TEXT("cache miss should report uncached execution"), bUsedCachedArtifact);
	TestEqual(TEXT("cache miss should build exactly one artifact"), Indexer.BuildArtifactCallCount, 1);
	TestEqual(TEXT("cache miss should materialize exactly one artifact"), Indexer.MaterializeCallCount, 1);
	TestEqual(TEXT("cache miss should write built artifact back once"), Cache.PutCallCount, 1);
	TestEqual(TEXT("database should contain built payload"), DB.ReadMeta(TEXT("test_global_pipeline_payload")), FString(TEXT("build-payload")));

	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("identity should build for cache lookup"), Indexer.BuildGlobalArtifactIdentity(Identity));
	const TOptional<FMonolithArtifact> CachedArtifact = Cache.Find(Identity);
	TestTrue(TEXT("built artifact should be written back to cache"), CachedArtifact.IsSet());
	TestEqual(
		TEXT("written-back artifact should match built payload"),
		CachedArtifact.IsSet() ? MonolithGlobalArtifactPipelineTestInternal::DecodePayload(CachedArtifact->Payload) : FString(),
		FString(TEXT("build-payload")));
	return true;
}
