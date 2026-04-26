#include "Indexers/AssetVisualGeometricIndexer.h"

#include "Embedders/GeometricEmbeddingProvider.h"
#include "AssetVisualArtifact.h"
#include "AssetVisualEmbeddingProvider.h"
#include "AssetVisualSharding.h"
#include "MonolithCaptureUtils.h"
#include "MonolithIndexerShadowMode.h"
#include "MonolithIndexLog.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"

/*
 * Geometric indexer 实现：
 *  1. 渲染 1 张 iso color view + silhouette（与查询路径吃同样的输入）
 *  2. 通过 registry 找到 geometric provider 调 Encode 算 64 维向量
 *  3. 把 iso color 编码成 PNG 字节
 *  4. 计算 ShardKey
 *  5. 用 AssetVisualArtifactSerializer 落 payload
 *
 * 单视图设计的核心理由：索引/查询必须吃同样的输入、走同一个 Encode、产出同一空间向量。
 * 否则 cosine 比较没有数学意义。
 */
namespace AssetVisualGeometricIndexerInternal
{
	using namespace MonolithCapture;

	/** 把 BGRA8 sRGB 图像编码成 PNG 字节流；失败返回空数组。 */
	static void EncodeImageToPng(const FImage& Image, TArray<uint8>& OutPngBytes)
	{
		OutPngBytes.Reset();
		if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.RawData.Num() == 0)
		{
			return;
		}

		IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> PngWrapper = WrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!PngWrapper.IsValid())
		{
			UE_LOG(LogMonolithIndex, Error, TEXT("AssetVisualGeometricIndexer: 无法创建 PNG wrapper"));
			return;
		}
		if (!PngWrapper->SetRaw(Image.RawData.GetData(), Image.RawData.Num(), Image.SizeX, Image.SizeY, ERGBFormat::BGRA, 8))
		{
			UE_LOG(LogMonolithIndex, Error, TEXT("AssetVisualGeometricIndexer: PNG SetRaw 失败"));
			return;
		}
		const TArray64<uint8> Compressed = PngWrapper->GetCompressed(85);
		OutPngBytes.Append(Compressed.GetData(), Compressed.Num());
	}

	/** 把 PNG 字节流写到本地磁盘；返回最终绝对路径。
	 *  落位规则：Saved/MonolithAssetVisual/{ShardId}/{资产短名}.png */
	static FString WritePreviewPngToDisk(
		const FString& AssetPath,
		const FString& ShardId,
		const TArray<uint8>& PngBytes)
	{
		const FString AssetName = FPaths::GetBaseFilename(AssetPath);
		const FString Dir = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("MonolithAssetVisual"),
			ShardId);
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString FilePath = FPaths::Combine(Dir, AssetName + TEXT(".png"));
		if (PngBytes.Num() > 0)
		{
			FFileHelper::SaveArrayToFile(PngBytes, *FilePath);
		}
		return FilePath;
	}

	/** 把 float[] embedding 转成 FP32 字节序列（little-endian 等价于直接 memcpy）。 */
	static void EncodeEmbeddingToBytes(const TArray<float>& Embedding, TArray<uint8>& OutBytes)
	{
		OutBytes.SetNumUninitialized(Embedding.Num() * sizeof(float));
		FMemory::Memcpy(OutBytes.GetData(), Embedding.GetData(), Embedding.Num() * sizeof(float));
	}
}

bool FAssetVisualGeometricIndexer::BuildArtifact(
	const FAssetData& AssetData,
	UObject* LoadedAsset,
	IAssetRegistry& AssetRegistry,
	FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	if (LoadedAsset == nullptr)
	{
		// 调用方未加载资产时 indexer 拒绝构建——commandlet 会按需先加载再 retry。
		return false;
	}

	// 4 类资产共用同一份 canonical iso 渲染：StaticMesh / SkeletalMesh / Material / WidgetBlueprint。
	// 真正的类型分发在 IAssetCanonicalRenderer 内部完成，这里只是先 reject 不支持的类。
	const bool bSupportedClass =
		LoadedAsset->IsA(UStaticMesh::StaticClass()) ||
		LoadedAsset->IsA(USkeletalMesh::StaticClass()) ||
		LoadedAsset->IsA(UMaterialInterface::StaticClass()) ||
		LoadedAsset->IsA(UWidgetBlueprint::StaticClass());
	if (!bSupportedClass)
	{
		return false;
	}

	using namespace MonolithCapture;
	using namespace AssetVisualGeometricIndexerInternal;

	// 拿全局 registry 中的 geometric provider；启动期由 subsystem 注册一份单例。
	const FName ProviderId = FName(TEXT("geometric_v1"));
	const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(ProviderId);
	if (!Provider.IsValid() || !Provider->IsAvailable())
	{
		UE_LOG(LogMonolithIndex, Error,
			TEXT("AssetVisualGeometricIndexer: provider 未注册或不可用"));
		return false;
	}

	// 1) 渲染 iso 单视图 + silhouette。
	IAssetCanonicalRenderer& Renderer = GetAssetCanonicalRenderer();
	FCanonicalRenderRequest Request;
	Request.Asset = LoadedAsset;
	Request.Resolution = 256;
	Request.Views = { ECanonicalView::Iso };
	Request.bComputeSilhouette = true;

	TArray<FCanonicalRenderResult> Results;
	if (!Renderer.RenderCanonical(Request, Results) || Results.Num() != 1)
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("AssetVisualGeometricIndexer: 渲染 iso 视图失败 (%s)"), *AssetData.PackageName.ToString());
		return false;
	}

	// 2) 算 64 维 embedding（与查询路径走同一份 Encode）。
	TArray<float> Embedding;
	if (!Provider->Encode(Results[0].ColorImage, Results[0].SilhouetteImage, Embedding))
	{
		return false;
	}

	// 3) 把 iso color 编码成 PNG 字节，artifact payload 携带这一份字节。
	TArray<uint8> PreviewPng;
	EncodeImageToPng(Results[0].ColorImage, PreviewPng);

	// 4) 计算 ShardKey（geometric 默认 InitialPrefixDepth=2；reducer 阶段再做容量再拆）。
	const FAssetVisualShardCapacityPolicy Policy = FAssetVisualShardCapacityPolicy::Geometric();
	const FString AssetPath = AssetData.GetObjectPathString();
	const FAssetVisualShardKey ShardKey = ComputeAssetVisualShardKey(AssetPath, Policy.InitialPrefixDepth);

	// 5) 装配视觉行 + 序列化 payload。
	const FAssetVisualProviderInfo ProviderInfo = Provider->GetProviderInfo();
	FIndexedAssetVisualEntry Entry;
	Entry.AssetPath = AssetPath;
	Entry.ShardId = ShardKey.ShardId;
	Entry.ShardPrefixDepth = ShardKey.PrefixDepth;
	Entry.ProviderId = ProviderInfo.ProviderId.ToString();
	Entry.ProviderVersion = ProviderInfo.ProviderVersion;
	Entry.RenderRecipeVersion = Renderer.GetRenderRecipeVersion();
	Entry.EmbeddingDim = Embedding.Num();
	Entry.EmbeddingDtype = 0; // FP32
	EncodeEmbeddingToBytes(Embedding, Entry.EmbeddingBytes);

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	AssetVisualArtifactSerializer::SerializePayload(Entry, PreviewPng, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FAssetVisualGeometricIndexer::MaterializeArtifact(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	const int64 AssetId)
{
	FIndexedAssetVisualEntry Entry;
	TArray<uint8> PreviewPng;
	if (!AssetVisualArtifactSerializer::DeserializePayload(Artifact.Payload, Entry, PreviewPng))
	{
		return false;
	}

	Entry.AssetId = AssetId;
	Entry.PreviewViewPath = AssetVisualGeometricIndexerInternal::WritePreviewPngToDisk(Entry.AssetPath, Entry.ShardId, PreviewPng);
	return DB.InsertAssetVisualEntry(TEXT("AssetVisualGeometric"), Entry) > 0;
}

bool FAssetVisualGeometricIndexer::MaterializeArtifactToShadow(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	const int64 AssetId,
	const FString& CohortName)
{
	FIndexedAssetVisualEntry Entry;
	TArray<uint8> PreviewPng;
	if (!AssetVisualArtifactSerializer::DeserializePayload(Artifact.Payload, Entry, PreviewPng))
	{
		return false;
	}

	Entry.AssetId = AssetId;
	Entry.PreviewViewPath = AssetVisualGeometricIndexerInternal::WritePreviewPngToDisk(Entry.AssetPath, Entry.ShardId, PreviewPng);

	FMonolithShadowIndexedAssetVisualEntry ShadowEntry;
	ShadowEntry.Entry = Entry;
	ShadowEntry.RowHash = ComputeAssetVisualRowHash(Entry);

	TArray<FMonolithShadowIndexedAssetVisualEntry> ShadowEntries;
	ShadowEntries.Add(MoveTemp(ShadowEntry));
	return DB.ReplaceShadowAssetVisualEntriesForAsset(
		TEXT("AssetVisualGeometric"),
		CohortName,
		AssetId,
		ShadowEntries);
}
