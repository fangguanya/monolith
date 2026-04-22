#include "MonolithArtifactCache.h"

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "MonolithArtifactCacheBreaker.h"

/*
 * 这组测试分别覆盖两件事：
 * - DDC cache 至少能做最基础的本地 round-trip；
 * - 新的 V2 编码需要守住 chunked / oversized 两条尺寸语义；
 * - breaker 会在连续失败后打开，并能通过半开探测恢复。
 */

namespace MonolithArtifactCacheDdcTestInternal
{
	/** 每个测试都生成独立 identity，避免被历史 DDC 残留污染。 */
	static FString MakeUniqueFingerprint()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	/** 用固定伪随机序列生成近似不可压缩 payload，方便稳定触发 chunk/oversized 分支。 */
	static void BuildDeterministicPayload(const int32 ByteCount, TArray<uint8>& OutPayload)
	{
		OutPayload.SetNumUninitialized(ByteCount);

		uint32 State = 0x13579BDFu;
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			State = State * 1664525u + 1013904223u;
			OutPayload[Index] = static_cast<uint8>(State >> 24);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheLocalRoundTripTest,
	"Monolith.Index.ArtifactCache.LocalRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheLocalRoundTripTest::RunTest(const FString& Parameters)
{
	// 这里不关心远端，只验证本地 DDC 的最小闭环。
	FMonolithDdcArtifactCache Cache;
	Cache.ResetStats();

	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("GenericAsset"));
	Identity.PackageName = FName(TEXT("/Game/Test/Texture_A"));
	Identity.PackageFingerprint = TEXT("roundtrip");

	FMonolithArtifact Artifact;
	Artifact.IndexerId = Identity.IndexerId;
	Artifact.PackageName = Identity.PackageName.ToString();
	Artifact.Payload = { 1, 2, 3, 4 };

	const bool bPutOk = Cache.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background);
	TestTrue(TEXT("remote write queue should drain cleanly after local round-trip"), Cache.DrainRemoteWrites(5.0));
	const TOptional<FMonolithArtifact> LoadedArtifact = Cache.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	const FMonolithArtifactCacheStats Stats = Cache.GetStats();

	TestTrue(TEXT("artifact put should succeed against local DDC"), bPutOk);
	TestTrue(TEXT("artifact should round-trip from DDC"), LoadedArtifact.IsSet());
	TestEqual(TEXT("payload should survive round-trip"), LoadedArtifact.IsSet() ? LoadedArtifact->Payload.Num() : 0, 4);
	TestTrue(TEXT("local hit counter should increase after round-trip"), Stats.LocalHitCount >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheChunkedRoundTripTest,
	"Monolith.Index.ArtifactCache.ChunkedRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheChunkedRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FMonolithDdcArtifactCache Cache;
	Cache.ResetStats();

	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("GenericAsset"));
	Identity.PackageName = FName(TEXT("/Game/Test/Texture_Chunked"));
	Identity.PackageFingerprint = MonolithArtifactCacheDdcTestInternal::MakeUniqueFingerprint();

	FMonolithArtifact Artifact;
	Artifact.IndexerId = Identity.IndexerId;
	Artifact.PackageName = Identity.PackageName.ToString();
	MonolithArtifactCacheDdcTestInternal::BuildDeterministicPayload(5 * 1024 * 1024, Artifact.Payload);

	TestTrue(TEXT("chunked artifact put should succeed"), Cache.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background));
	TestTrue(TEXT("chunked remote write queue should drain cleanly"), Cache.DrainRemoteWrites(10.0));

	const TOptional<FMonolithArtifact> LoadedArtifact = Cache.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	TestTrue(TEXT("chunked artifact should round-trip from local DDC"), LoadedArtifact.IsSet());
	TestEqual(
		TEXT("chunked payload size should survive round-trip"),
		LoadedArtifact.IsSet() ? LoadedArtifact->Payload.Num() : 0,
		Artifact.Payload.Num());
	TestTrue(
		TEXT("chunked payload bytes should stay identical"),
		LoadedArtifact.IsSet() && LoadedArtifact->Payload == Artifact.Payload);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheOversizedArtifactTest,
	"Monolith.Index.ArtifactCache.OversizedArtifactsSkipCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheOversizedArtifactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FMonolithDdcArtifactCache Cache;
	Cache.ResetStats();

	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("GenericAsset"));
	Identity.PackageName = FName(TEXT("/Game/Test/Texture_Oversized"));
	Identity.PackageFingerprint = MonolithArtifactCacheDdcTestInternal::MakeUniqueFingerprint();

	FMonolithArtifact Artifact;
	Artifact.IndexerId = Identity.IndexerId;
	Artifact.PackageName = Identity.PackageName.ToString();
	MonolithArtifactCacheDdcTestInternal::BuildDeterministicPayload(17 * 1024 * 1024, Artifact.Payload);

	TestTrue(
		TEXT("oversized artifact should not fail the caller even though it skips cache storage"),
		Cache.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background));
	TestTrue(TEXT("oversized path should not leave remote writes pending"), Cache.DrainRemoteWrites(5.0));

	const TOptional<FMonolithArtifact> LoadedArtifact = Cache.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	const FMonolithArtifactCacheStats Stats = Cache.GetStats();

	TestFalse(TEXT("oversized artifact should not be readable from DDC because it was never cached"), LoadedArtifact.IsSet());
	TestEqual(TEXT("oversized stat should increment once"), Stats.OversizedArtifactCount, 1ull);
	TestEqual(TEXT("oversized artifact should not enqueue remote writes"), Stats.RemoteWriteOkCount, 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheBreakerOpenTest,
	"Monolith.Index.ArtifactCache.BreakerOpensAfterConsecutiveErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheBreakerOpenTest::RunTest(const FString& Parameters)
{
	// 连续三次失败后，breaker 应该进入打开状态。
	FMonolithArtifactCacheBreaker Breaker(60.0, 3, 8, 30.0);
	Breaker.RecordFailure(1.0);
	Breaker.RecordFailure(2.0);
	Breaker.RecordFailure(3.0);

	TestTrue(TEXT("breaker should open after threshold"), Breaker.IsOpen(3.0));
	TestFalse(TEXT("open breaker should reject get probes"), Breaker.AllowGet(3.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheBreakerHalfOpenRecoveryTest,
	"Monolith.Index.ArtifactCache.HalfOpenProbeRecoversRemotePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheBreakerHalfOpenRecoveryTest::RunTest(const FString& Parameters)
{
	// 模拟：先失败打开 breaker，再等半开窗口，最后用 get/put probe 恢复。
	FMonolithArtifactCacheBreaker Breaker(10.0, 2, 8, 30.0);
	Breaker.RecordFailure(1.0);
	Breaker.RecordFailure(2.0);

	TestTrue(TEXT("breaker should be open immediately after failures"), Breaker.IsOpen(2.0));
	TestTrue(TEXT("get probe should be allowed after open interval"), Breaker.AllowGet(12.5));
	Breaker.RecordRemoteGetSuccess(12.5);
	TestTrue(TEXT("put probe should be allowed after get probe succeeds"), Breaker.AllowPut(12.5));
	Breaker.RecordRemotePutSuccess(12.5);

	TestFalse(TEXT("successful probes should close the breaker"), Breaker.IsOpen(12.6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactCacheDiscardPendingRemoteWritesTest,
	"Monolith.Index.ArtifactCache.DiscardPendingRemoteWritesKeepsLocalSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactCacheDiscardPendingRemoteWritesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FMonolithDdcArtifactCache Cache;
	Cache.ResetStats();

	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("GenericAsset"));
	Identity.PackageName = FName(TEXT("/Game/Test/Texture_DiscardRemote"));
	Identity.PackageFingerprint = TEXT("discard_remote");

	FMonolithArtifact Artifact;
	Artifact.IndexerId = Identity.IndexerId;
	Artifact.PackageName = Identity.PackageName.ToString();
	Artifact.Payload = { 9, 8, 7, 6 };

	TestTrue(
		TEXT("artifact put should still succeed before remote discard"),
		Cache.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background));

	// 这里专门覆盖“退出时只丢远端队列，但本地快照仍可用”的语义。
	Cache.DiscardPendingRemoteWrites();
	TestTrue(TEXT("discarding queued remote writes should leave cache in a drained state"), Cache.DrainRemoteWrites(5.0));

	const TOptional<FMonolithArtifact> LoadedArtifact = Cache.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	TestTrue(TEXT("local snapshot should remain readable after dropping pending remote writes"), LoadedArtifact.IsSet());
	TestEqual(TEXT("local snapshot payload should stay intact"), LoadedArtifact.IsSet() ? LoadedArtifact->Payload.Num() : 0, 4);
	return true;
}
