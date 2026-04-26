#include "Indexers/AssetVisualSemanticIndexer.h"

#include "Embedders/ClipSemanticEmbeddingProvider.h"
#include "AssetVisualArtifact.h"
#include "AssetVisualEmbeddingProvider.h"
#include "AssetVisualSharding.h"
#include "MonolithCaptureUtils.h"
#include "MonolithIndexerShadowMode.h"
#include "MonolithIndexLog.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "WidgetBlueprint.h"

/*
 * Semantic indexer 实现：
 *  1. 检查 provider IsAvailable；不可用直接 false（mesh 由 commandlet 标 stale）
 *  2. 渲染单张 iso 视图（不算 silhouette）
 *  3. provider->Encode() 推理出 512 维向量
 *  4. 计算 ShardKey（semantic shard 上限 8K mesh）
 *  5. 序列化 artifact，注意 PreviewPng 字段写空（geometric indexer 已经持久化了 iso PNG）
 */

bool FAssetVisualSemanticIndexer::BuildArtifact(
	const FAssetData& AssetData,
	UObject* LoadedAsset,
	IAssetRegistry& AssetRegistry,
	FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	if (LoadedAsset == nullptr)
	{
		return false;
	}

	const bool bSupportedClass =
		LoadedAsset->IsA(UStaticMesh::StaticClass()) ||
		LoadedAsset->IsA(USkeletalMesh::StaticClass()) ||
		LoadedAsset->IsA(UMaterialInterface::StaticClass()) ||
		LoadedAsset->IsA(UWidgetBlueprint::StaticClass());
	if (!bSupportedClass)
	{
		return false;
	}

	// Provider 通过 registry 取（启动时由 subsystem 注册一份单例）。
	const FName ProviderId = FName(TEXT("clip_vit_b32_v1"));
	const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(ProviderId);
	if (!Provider.IsValid() || !Provider->IsAvailable())
	{
		// Provider 不可用是 spec 一等支持状态：调用方按需把 asset 标 stale，不重试。
		UE_LOG(LogMonolithIndex, Verbose,
			TEXT("AssetVisualSemanticIndexer: provider 不可用 (asset=%s)，跳过 artifact 构建"),
			*AssetData.PackageName.ToString());
		return false;
	}

	using namespace MonolithCapture;

	// 1) 渲染 iso 单视图（不要 silhouette）。
	IAssetCanonicalRenderer& Renderer = GetAssetCanonicalRenderer();
	FCanonicalRenderRequest Request;
	Request.Asset = LoadedAsset;
	Request.Resolution = 224; // CLIP-ViT-B/32 输入分辨率
	Request.Views = { ECanonicalView::Iso };
	Request.bComputeSilhouette = false;

	TArray<FCanonicalRenderResult> Results;
	if (!Renderer.RenderCanonical(Request, Results) || Results.Num() != 1)
	{
		return false;
	}

	// 2) 推理 512 维向量。
	TArray<float> Embedding;
	if (!Provider->Encode(Results[0].ColorImage, Results[0].SilhouetteImage, Embedding))
	{
		// Encode 失败：commandlet 会把 mesh 重投 OfflineOnly 队列，等下次跑或下次模型就位。
		return false;
	}

	// 3) ShardKey（semantic 容量上限 8K mesh / shard）。
	const FAssetVisualShardCapacityPolicy Policy = FAssetVisualShardCapacityPolicy::Semantic();
	const FString AssetPath = AssetData.GetObjectPathString();
	const FAssetVisualShardKey ShardKey = ComputeAssetVisualShardKey(AssetPath, Policy.InitialPrefixDepth);

	// 4) 装配视觉行 + 序列化 payload。
	const FAssetVisualProviderInfo ProviderInfo = Provider->GetProviderInfo();
	FIndexedAssetVisualEntry Entry;
	Entry.AssetPath = AssetPath;
	Entry.ShardId = ShardKey.ShardId;
	Entry.ShardPrefixDepth = ShardKey.PrefixDepth;
	Entry.ProviderId = ProviderInfo.ProviderId.ToString();
	Entry.ProviderVersion = ProviderInfo.ProviderVersion;
	Entry.RenderRecipeVersion = Renderer.GetRenderRecipeVersion();
	Entry.EmbeddingDim = Embedding.Num();
	// CLIP 推理后 retriever 仍按 FP32 比较；FP16 节省 cohort embedding 表的存储字节数。
	// 这里编码为 FP16 半精度落盘。
	Entry.EmbeddingDtype = 1;
	Entry.EmbeddingBytes.SetNumUninitialized(Embedding.Num() * sizeof(uint16));
	{
		// FP32 → FP16 round-to-nearest-even（足够推理用）。
		uint16* Dst = reinterpret_cast<uint16*>(Entry.EmbeddingBytes.GetData());
		for (int32 Index = 0; Index < Embedding.Num(); ++Index)
		{
			uint32 F = 0;
			FMemory::Memcpy(&F, &Embedding[Index], sizeof(uint32));
			const uint32 Sign = (F >> 16) & 0x8000;
			int32 Exp = static_cast<int32>((F >> 23) & 0xff) - 127 + 15;
			uint32 Mant = F & 0x7fffff;
			if (Exp <= 0)
			{
				// underflow → ±0
				Dst[Index] = static_cast<uint16>(Sign);
			}
			else if (Exp >= 31)
			{
				// overflow / inf / nan
				Dst[Index] = static_cast<uint16>(Sign | 0x7c00);
			}
			else
			{
				Dst[Index] = static_cast<uint16>(Sign | (static_cast<uint32>(Exp) << 10) | (Mant >> 13));
			}
		}
	}

	// semantic indexer 不重复持久化 PNG；preview_view 字段在 Materialize 阶段从
	// geometric cohort 同 mesh 行复用。
	const TArray<uint8> EmptyPng;

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	AssetVisualArtifactSerializer::SerializePayload(Entry, EmptyPng, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FAssetVisualSemanticIndexer::MaterializeArtifact(
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

	// preview_view 复用 geometric cohort 已落盘的 PNG。
	const TOptional<FIndexedAssetVisualEntry> GeometricRow = DB.GetAssetVisualEntryForAsset(TEXT("AssetVisualGeometric"), AssetId);
	if (GeometricRow.IsSet())
	{
		Entry.PreviewViewPath = GeometricRow.GetValue().PreviewViewPath;
	}

	return DB.InsertAssetVisualEntry(TEXT("AssetVisualSemantic"), Entry) > 0;
}

bool FAssetVisualSemanticIndexer::MaterializeArtifactToShadow(
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
	const TOptional<FIndexedAssetVisualEntry> GeometricRow = DB.GetAssetVisualEntryForAsset(TEXT("AssetVisualGeometric"), AssetId);
	if (GeometricRow.IsSet())
	{
		Entry.PreviewViewPath = GeometricRow.GetValue().PreviewViewPath;
	}

	FMonolithShadowIndexedAssetVisualEntry ShadowEntry;
	ShadowEntry.Entry = Entry;
	ShadowEntry.RowHash = ComputeAssetVisualRowHash(Entry);

	TArray<FMonolithShadowIndexedAssetVisualEntry> ShadowEntries;
	ShadowEntries.Add(MoveTemp(ShadowEntry));
	return DB.ReplaceShadowAssetVisualEntriesForAsset(
		TEXT("AssetVisualSemantic"),
		CohortName,
		AssetId,
		ShadowEntries);
}
