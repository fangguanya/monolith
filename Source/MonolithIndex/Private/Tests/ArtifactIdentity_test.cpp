#include "MonolithArtifactTypes.h"

#include "Misc/AutomationTest.h"
#include "MonolithSettings.h"

/*
 * 这组测试守住 artifact identity 的两个基本前提：
 * 1. 同样输入必须生成同样字节和同样哈希；
 * 2. 配置切换 provider 时，读取逻辑要跟着切换。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactIdentityDeterministicTest,
	"Monolith.Index.ArtifactIdentity.DeterministicSingleProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactIdentityDeterministicTest::RunTest(const FString& Parameters)
{
	// 构造一份字段比较全的 identity，验证序列化和哈希都稳定。
	FMonolithArtifactIdentityV1 Identity;
	Identity.IndexerId = FName(TEXT("GenericAsset"));
	Identity.IndexerVersion = 7;
	Identity.ArtifactSchemaVersion = 3;
	Identity.PackageName = FName(TEXT("/Game/Test/SM_Test"));
	Identity.IdentityProvider = EMonolithIdentityProvider::SavedHash;
	Identity.PackageFingerprint = TEXT("0123456789abcdef");
	Identity.EngineMajorVersion = 5;
	Identity.EngineMinorVersion = 7;
	Identity.DependencyVersions = {
		{ FName(TEXT("Renderer")), 2 },
		{ FName(TEXT("Anim")), 1 },
		{ FName(TEXT("Renderer")), 1 },
	};

	const TArray<uint8> FirstBytes = SerializeMonolithArtifactIdentity(Identity);
	const TArray<uint8> SecondBytes = SerializeMonolithArtifactIdentity(Identity);
	const FIoHash FirstHash = HashMonolithArtifactIdentity(Identity);
	const FIoHash SecondHash = HashMonolithArtifactIdentity(Identity);

	TestEqual(TEXT("serialization should be deterministic"), FirstBytes, SecondBytes);
	TestEqual(TEXT("identity hash should be deterministic"), LexToString(FirstHash), LexToString(SecondHash));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithArtifactIdentityProviderSwitchTest,
	"Monolith.Index.ArtifactIdentity.ProviderSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithArtifactIdentityProviderSwitchTest::RunTest(const FString& Parameters)
{
	// 直接改默认设置对象，模拟 ini 里切换 provider。
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("settings default object should exist"), Settings);

	const FString PreviousValue = Settings->IndexIdentityProvider;
	Settings->IndexIdentityProvider = TEXT("ARSnapshotV1");

	const EMonolithIdentityProvider Provider = GetConfiguredMonolithIdentityProvider();
	TestEqual(TEXT("configured provider should switch to AR snapshot"), Provider, EMonolithIdentityProvider::ARSnapshotV1);

	Settings->IndexIdentityProvider = PreviousValue;
	return true;
}
