#include "Indexers/AssetVisualGeometricIndexer.h"

#include "Embedders/GeometricEmbeddingProvider.h"
#include "Indexers/AssetVisualPhase.h"
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
	 *  单 phase 资产落位：Saved/MonolithAssetVisual/{ShardId}/{资产短名}.png
	 *  多 phase 资产落位：Saved/MonolithAssetVisual/{ShardId}/{资产短名}_p{PhaseId}.png
	 *  PhaseId=0 + 单 phase 默认（PhaseLabel 空 + 单元素）走无后缀路径，与历史行为兼容。 */
	static FString WritePreviewPngToDisk(
		const FString& AssetPath,
		const FString& ShardId,
		const TArray<uint8>& PngBytes,
		const uint8 PhaseId = 0,
		const bool bMultiPhase = false)
	{
		const FString AssetName = FPaths::GetBaseFilename(AssetPath);
		const FString Dir = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("MonolithAssetVisual"),
			ShardId);
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString FileBase = bMultiPhase
			? FString::Printf(TEXT("%s_p%u"), *AssetName, static_cast<uint32>(PhaseId))
			: AssetName;
		const FString FilePath = FPaths::Combine(Dir, FileBase + TEXT(".png"));
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

	// 不再做硬编码 IsA 检查；canonical renderer 内部用 UThumbnailManager 自动分发，
	// 它支持任何注册了 ThumbnailRenderer 的资产类（StaticMesh/SkelMesh/Material/Widget/Niagara/Anim 都自带）。
	// 没注册 renderer 的资产类 renderer 会返 false。

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

	// Multi-phase 索引：用 GetPhasesForAsset 决定该资产分几行 + 每行用什么时间点采样。
	// AnimSequence / NiagaraSystem 走 3 phase，其他走单 phase。
	const TArray<FAssetVisualPhaseDef> Phases = AssetVisualPhase::GetPhasesForAsset(LoadedAsset);
	if (Phases.Num() == 0)
	{
		// GetPhasesForAsset 至少返回 1；为 0 是逻辑 bug。
		return false;
	}
	const FName ClassHint = AssetVisualPhase::GetRendererAssetClassHint(LoadedAsset);

	IAssetCanonicalRenderer& Renderer = GetAssetCanonicalRenderer();

	// ShardKey 与资产路径绑定，所有 phase 共享同一 ShardId。
	const FAssetVisualShardCapacityPolicy Policy = FAssetVisualShardCapacityPolicy::Geometric();
	const FString AssetPath = AssetData.GetObjectPathString();
	const FAssetVisualShardKey ShardKey = ComputeAssetVisualShardKey(AssetPath, Policy.InitialPrefixDepth);
	const FAssetVisualProviderInfo ProviderInfo = Provider->GetProviderInfo();

	TArray<FIndexedAssetVisualEntry> Entries;
	TArray<TArray<uint8>> PerPhasePngs;
	Entries.Reserve(Phases.Num());
	PerPhasePngs.Reserve(Phases.Num());

	for (const FAssetVisualPhaseDef& Phase : Phases)
	{
		FCanonicalRenderRequest Request;
		Request.Asset = LoadedAsset;
		Request.Resolution = 256;
		Request.Views = { ECanonicalView::Iso };
		Request.bComputeSilhouette = true;
		Request.PhaseT = Phase.PhaseT;
		Request.PhaseId = Phase.PhaseId;
		Request.AssetClassHint = ClassHint;

		TArray<FCanonicalRenderResult> Results;
		if (!Renderer.RenderCanonical(Request, Results) || Results.Num() != 1)
		{
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("AssetVisualGeometricIndexer: 渲染 phase %u (t=%.3f) 失败 (%s)"),
				static_cast<uint32>(Phase.PhaseId), Phase.PhaseT, *AssetData.PackageName.ToString());
			return false;
		}

		TArray<float> Embedding;
		if (!Provider->Encode(Results[0].ColorImage, Results[0].SilhouetteImage, Embedding))
		{
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("AssetVisualGeometricIndexer: phase %u Encode 失败 (%s)"),
				static_cast<uint32>(Phase.PhaseId), *AssetData.PackageName.ToString());
			return false;
		}

		FIndexedAssetVisualEntry Entry;
		Entry.AssetPath = AssetPath;
		Entry.ShardId = ShardKey.ShardId;
		Entry.ShardPrefixDepth = ShardKey.PrefixDepth;
		Entry.ProviderId = ProviderInfo.ProviderId.ToString();
		Entry.ProviderVersion = ProviderInfo.ProviderVersion;
		Entry.RenderRecipeVersion = Renderer.GetRenderRecipeVersion();
		Entry.EmbeddingDim = Embedding.Num();
		Entry.EmbeddingDtype = 0; // FP32
		Entry.PhaseId = Phase.PhaseId;
		Entry.PhaseT = Phase.PhaseT;
		Entry.PhaseLabel = Phase.Label;
		EncodeEmbeddingToBytes(Embedding, Entry.EmbeddingBytes);

		TArray<uint8> PhasePng;
		EncodeImageToPng(Results[0].ColorImage, PhasePng);

		Entries.Add(MoveTemp(Entry));
		PerPhasePngs.Add(MoveTemp(PhasePng));
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	AssetVisualArtifactSerializer::SerializePayload(Entries, PerPhasePngs, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FAssetVisualGeometricIndexer::MaterializeArtifact(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	const int64 AssetId)
{
	TArray<FIndexedAssetVisualEntry> Entries;
	TArray<TArray<uint8>> PerPhasePngs;
	if (!AssetVisualArtifactSerializer::DeserializePayload(Artifact.Payload, Entries, PerPhasePngs))
	{
		return false;
	}
	if (Entries.Num() == 0 || PerPhasePngs.Num() != Entries.Num())
	{
		return false;
	}

	// Multi-phase 资产 N>1 时每 phase 一张 PNG（文件名带 _pN 后缀）；单 phase 资产 N=1 走无后缀路径。
	const bool bMultiPhase = Entries.Num() > 1;

	bool bAllOk = true;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		FIndexedAssetVisualEntry& Entry = Entries[Index];
		Entry.AssetId = AssetId;
		Entry.PreviewViewPath = AssetVisualGeometricIndexerInternal::WritePreviewPngToDisk(
			Entry.AssetPath, Entry.ShardId, PerPhasePngs[Index], Entry.PhaseId, bMultiPhase);
		if (DB.InsertAssetVisualEntry(TEXT("AssetVisualGeometric"), Entry) <= 0)
		{
			bAllOk = false;
		}
	}
	return bAllOk;
}

bool FAssetVisualGeometricIndexer::MaterializeArtifactToShadow(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	const int64 AssetId,
	const FString& CohortName)
{
	TArray<FIndexedAssetVisualEntry> Entries;
	TArray<TArray<uint8>> PerPhasePngs;
	if (!AssetVisualArtifactSerializer::DeserializePayload(Artifact.Payload, Entries, PerPhasePngs))
	{
		return false;
	}
	if (Entries.Num() == 0 || PerPhasePngs.Num() != Entries.Num())
	{
		return false;
	}

	const bool bMultiPhase = Entries.Num() > 1;

	TArray<FMonolithShadowIndexedAssetVisualEntry> ShadowEntries;
	ShadowEntries.Reserve(Entries.Num());
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		FIndexedAssetVisualEntry& Entry = Entries[Index];
		Entry.AssetId = AssetId;
		Entry.PreviewViewPath = AssetVisualGeometricIndexerInternal::WritePreviewPngToDisk(
			Entry.AssetPath, Entry.ShardId, PerPhasePngs[Index], Entry.PhaseId, bMultiPhase);

		FMonolithShadowIndexedAssetVisualEntry ShadowEntry;
		ShadowEntry.Entry = Entry;
		ShadowEntry.RowHash = ComputeAssetVisualRowHash(Entry);
		ShadowEntries.Add(MoveTemp(ShadowEntry));
	}

	return DB.ReplaceShadowAssetVisualEntriesForAsset(
		TEXT("AssetVisualGeometric"),
		CohortName,
		AssetId,
		ShadowEntries);
}
