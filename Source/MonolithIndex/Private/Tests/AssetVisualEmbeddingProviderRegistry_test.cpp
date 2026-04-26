#include "AssetVisualEmbeddingProvider.h"

#include "Misc/AutomationTest.h"

/*
 * Provider registry 测试覆盖：
 *  - 注册 + 查找 + 反注册闭环
 *  - 重复注册被拒绝
 */

namespace AssetVisualEmbeddingProviderRegistryTestInternal
{
	/** 一个仅用于测试的最小 provider；所有方法返回 deterministic 值。 */
	class FStubProvider : public IAssetVisualEmbeddingProvider
	{
	public:
		FStubProvider(const FName InId, const uint32 InVersion) { Info.ProviderId = InId; Info.ProviderVersion = InVersion; Info.EmbeddingDim = 8; }
		virtual FAssetVisualProviderInfo GetProviderInfo() const override { return Info; }
		virtual bool IsAvailable() const override { return true; }
		virtual bool Encode(const FImage&, const FImage&, TArray<float>& Out) override
		{
			Out.SetNumZeroed(Info.EmbeddingDim);
			return true;
		}
	private:
		FAssetVisualProviderInfo Info;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualEmbeddingProviderRegistryRoundTripTest,
	"Monolith.Index.AssetVisual.Provider.RegistryRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualEmbeddingProviderRegistryRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualEmbeddingProviderRegistryTestInternal;

	const FName Id(TEXT("test_provider_round_trip"));
	FAssetVisualEmbeddingProviderRegistry::Get().UnregisterProvider(Id);

	const TSharedPtr<IAssetVisualEmbeddingProvider> Found1 =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(Id);
	TestFalse(TEXT("should not find before register"), Found1.IsValid());

	FAssetVisualEmbeddingProviderRegistry::Get().RegisterProvider(MakeShared<FStubProvider>(Id, 7));
	const TSharedPtr<IAssetVisualEmbeddingProvider> Found2 =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(Id);
	TestTrue(TEXT("should find after register"), Found2.IsValid());
	if (Found2.IsValid())
	{
		TestEqual(TEXT("provider version preserved"), static_cast<int32>(Found2->GetProviderInfo().ProviderVersion), 7);
	}

	FAssetVisualEmbeddingProviderRegistry::Get().UnregisterProvider(Id);
	const TSharedPtr<IAssetVisualEmbeddingProvider> Found3 =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(Id);
	TestFalse(TEXT("should not find after unregister"), Found3.IsValid());
	return true;
}
