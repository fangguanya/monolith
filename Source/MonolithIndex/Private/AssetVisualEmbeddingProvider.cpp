#include "AssetVisualEmbeddingProvider.h"

#include "MonolithIndexLog.h"

/*
 * 全局 provider 注册表实现。
 *
 * 实现刻意保持极小：一个 TMap + FCriticalSection。
 * Provider 注册次数固定（启动一次、关闭一次），所以这里完全不需要 lock-free 容器。
 */
FAssetVisualEmbeddingProviderRegistry& FAssetVisualEmbeddingProviderRegistry::Get()
{
	static FAssetVisualEmbeddingProviderRegistry Instance;
	return Instance;
}

void FAssetVisualEmbeddingProviderRegistry::RegisterProvider(TSharedPtr<IAssetVisualEmbeddingProvider> Provider)
{
	if (!Provider.IsValid())
	{
		return;
	}

	const FAssetVisualProviderInfo Info = Provider->GetProviderInfo();
	if (Info.ProviderId.IsNone())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("AssetVisualEmbeddingProvider 注册失败：ProviderId 为空"));
		return;
	}

	FScopeLock Lock(&RegistryLock);
	if (Providers.Contains(Info.ProviderId))
	{
		// 同一 ProviderId 必须只注册一次；否则 cohort 的 stale 判定会变脆弱。
		UE_LOG(LogMonolithIndex, Error,
			TEXT("AssetVisualEmbeddingProvider '%s' 已被注册，拒绝重复注册"),
			*Info.ProviderId.ToString());
		ensureMsgf(false, TEXT("Duplicate provider registration: %s"), *Info.ProviderId.ToString());
		return;
	}

	Providers.Emplace(Info.ProviderId, MoveTemp(Provider));
	UE_LOG(LogMonolithIndex, Log,
		TEXT("AssetVisualEmbeddingProvider 已注册：%s v%u (dim=%d)"),
		*Info.ProviderId.ToString(), Info.ProviderVersion, Info.EmbeddingDim);
}

void FAssetVisualEmbeddingProviderRegistry::UnregisterProvider(const FName ProviderId)
{
	FScopeLock Lock(&RegistryLock);
	Providers.Remove(ProviderId);
}

TSharedPtr<IAssetVisualEmbeddingProvider> FAssetVisualEmbeddingProviderRegistry::FindProvider(const FName ProviderId) const
{
	FScopeLock Lock(&RegistryLock);
	const TSharedPtr<IAssetVisualEmbeddingProvider>* Found = Providers.Find(ProviderId);
	return Found ? *Found : nullptr;
}

TArray<FAssetVisualProviderInfo> FAssetVisualEmbeddingProviderRegistry::ListProviders() const
{
	TArray<FAssetVisualProviderInfo> Result;
	FScopeLock Lock(&RegistryLock);
	Result.Reserve(Providers.Num());
	for (const TPair<FName, TSharedPtr<IAssetVisualEmbeddingProvider>>& Pair : Providers)
	{
		if (Pair.Value.IsValid())
		{
			Result.Add(Pair.Value->GetProviderInfo());
		}
	}
	return Result;
}

void DeriveAssetVisualSilhouetteFromColor(const FImage& ColorImage, FImage& OutSilhouette)
{
	// 阈值 24 与 IAssetCanonicalRenderer / GeometricEmbeddingProvider 内部完全一致。
	constexpr uint32 SilhouetteForegroundLumaThreshold = 24;

	OutSilhouette.Init(ColorImage.SizeX, ColorImage.SizeY, ERawImageFormat::G8, EGammaSpace::Linear);
	const FColor* Src = reinterpret_cast<const FColor*>(ColorImage.RawData.GetData());
	uint8* Dst = OutSilhouette.RawData.GetData();
	const int32 PixelCount = ColorImage.SizeX * ColorImage.SizeY;
	for (int32 Index = 0; Index < PixelCount; ++Index)
	{
		const FColor& C = Src[Index];
		const uint32 Luma = static_cast<uint32>(C.R) + static_cast<uint32>(C.G) + static_cast<uint32>(C.B);
		Dst[Index] = (Luma > SilhouetteForegroundLumaThreshold) ? 0xFFu : 0x00u;
	}
}
