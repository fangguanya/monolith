#include "MonolithArtifactCache.h"

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

/*
 * Bucket 参数化测试覆盖：
 *  - 不同 bucket 的 cache 实例完全隔离（同 identity 在 A bucket 写入不会被 B bucket 读到）
 *  - 默认 bucket 与 AssetVisualGeometric / AssetVisualSemantic bucket 之间互不串用
 *
 * 这条隔离是 spec 的硬约束，必须有自动化兜底。
 */

namespace DdcCacheBucketParamTestInternal
{
	static FString MakeUniqueFingerprint()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDdcCacheBucketIsolationTest,
	"Monolith.Index.ArtifactCache.BucketIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDdcCacheBucketIsolationTest::RunTest(const FString& Parameters)
{
	using namespace DdcCacheBucketParamTestInternal;

	// 两个独立的 cache 实例，分别绑到 default 与 AssetVisualGeometric bucket。
	const FString BucketA = MonolithCacheBuckets::Default;
	const FString BucketB = MonolithCacheBuckets::AssetVisualGeometric;
	FMonolithDdcArtifactCache CacheA(BucketA);
	FMonolithDdcArtifactCache CacheB(BucketB);

	const FString Fingerprint = MakeUniqueFingerprint();

	// 同一份 identity 在 A bucket 写入。
	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("AssetVisualGeometric"));
	Identity.PackageName = FName(TEXT("/Game/Test/IsolationProbe"));
	Identity.PackageFingerprint = Fingerprint;

	FMonolithArtifact Artifact;
	Artifact.IndexerId = Identity.IndexerId;
	Artifact.PackageName = Identity.PackageName.ToString();
	Artifact.Payload = { 9, 8, 7, 6 };

	const bool bPutOk = CacheA.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background);
	TestTrue(TEXT("put to bucket A should drain remote queue"), CacheA.DrainRemoteWrites(5.0));

	// A bucket 应当能 Get 回这份 artifact。
	const TOptional<FMonolithArtifact> ReadFromA = CacheA.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	TestTrue(TEXT("artifact must be visible in same bucket A"), ReadFromA.IsSet());

	// B bucket 必须看不到这份 artifact（隔离）。
	const TOptional<FMonolithArtifact> ReadFromB = CacheB.Get(Identity, EMonolithArtifactCacheRequestMode::Background);
	TestFalse(TEXT("artifact must NOT leak across buckets"), ReadFromB.IsSet());

	// 收尾。
	(void)bPutOk;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDdcCacheBucketNamesDistinctTest,
	"Monolith.Index.ArtifactCache.BucketNamesDistinct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDdcCacheBucketNamesDistinctTest::RunTest(const FString& Parameters)
{
	const FString Default = FString(MonolithCacheBuckets::Default);
	const FString Geometric = FString(MonolithCacheBuckets::AssetVisualGeometric);
	const FString Semantic = FString(MonolithCacheBuckets::AssetVisualSemantic);

	TestNotEqual(TEXT("default vs geometric"), Default, Geometric);
	TestNotEqual(TEXT("default vs semantic"), Default, Semantic);
	TestNotEqual(TEXT("geometric vs semantic"), Geometric, Semantic);

	// 命名约定：AssetVisual 双 cohort 必须在 bucket name 里包含明确字段，便于运维一眼识别。
	TestTrue(TEXT("geometric bucket contains tag"), Geometric.Contains(TEXT("AssetVisualGeometric")));
	TestTrue(TEXT("semantic bucket contains tag"), Semantic.Contains(TEXT("AssetVisualSemantic")));
	return true;
}
