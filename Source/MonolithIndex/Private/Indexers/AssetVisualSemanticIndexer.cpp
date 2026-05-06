#include "Indexers/AssetVisualSemanticIndexer.h"

#include "Embedders/ClipSemanticEmbeddingProvider.h"
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
#include "HAL/ThreadSafeBool.h"
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

	// 不再做硬编码 IsA 检查；canonical renderer 内部用 UThumbnailManager 自动分发。

	// Provider 通过 registry 取（启动时由 subsystem 注册一份单例）。
	const FName ProviderId = FName(TEXT("clip_vit_b32_v1"));
	const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
		FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(ProviderId);
	if (!Provider.IsValid() || !Provider->IsAvailable())
	{
		// Provider 不可用是 spec 一等支持状态（ONNX 缺失 / NNE 不可用），不阻塞 geometric。
		// 但 24233 个资产全部静默 false 调试不友好——首次命中时打 Warning 让人立刻看到根因。
		// 之后单 process 内重复命中只在 Verbose 打日志，避免刷屏。
		static FThreadSafeBool bUnavailableWarned = false;
		if (!bUnavailableWarned.AtomicSet(true))
		{
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("AssetVisualSemanticIndexer: clip_vit_b32_v1 provider 不可用，整 cohort 跳过；"
				     "请检查 Plugins/Monolith/Resources/Models/clip_vit_b32_image_encoder_fp16.onnx 是否就位 + NNERuntimeORT 是否启用。"
				     "首个命中资产: %s"),
				*AssetData.PackageName.ToString());
		}
		else
		{
			UE_LOG(LogMonolithIndex, Verbose,
				TEXT("AssetVisualSemanticIndexer: provider 不可用 (asset=%s)，跳过"),
				*AssetData.PackageName.ToString());
		}
		return false;
	}

	using namespace MonolithCapture;

	// Multi-phase 索引：与 geometric indexer 同步用 GetPhasesForAsset 决定 phase 列表。
	// 必须共享同一份 phase 定义，否则 geometric / semantic 在同 PhaseId 下对应的不是同一时间点。
	const TArray<FAssetVisualPhaseDef> Phases = AssetVisualPhase::GetPhasesForAsset(LoadedAsset);
	if (Phases.Num() == 0)
	{
		return false;
	}
	const FName ClassHint = AssetVisualPhase::GetRendererAssetClassHint(LoadedAsset);

	IAssetCanonicalRenderer& Renderer = GetAssetCanonicalRenderer();

	// ShardKey 跨 phase 一致（semantic 容量上限 8K mesh / shard）。
	const FAssetVisualShardCapacityPolicy Policy = FAssetVisualShardCapacityPolicy::Semantic();
	const FString AssetPath = AssetData.GetObjectPathString();
	const FAssetVisualShardKey ShardKey = ComputeAssetVisualShardKey(AssetPath, Policy.InitialPrefixDepth);
	const FAssetVisualProviderInfo ProviderInfo = Provider->GetProviderInfo();

	TArray<FIndexedAssetVisualEntry> Entries;
	TArray<TArray<uint8>> EmptyPngs; // semantic 不携带 preview，每 phase 一份空 blob。
	Entries.Reserve(Phases.Num());
	EmptyPngs.Reserve(Phases.Num());

	for (const FAssetVisualPhaseDef& Phase : Phases)
	{
		FCanonicalRenderRequest Request;
		Request.Asset = LoadedAsset;
		Request.Resolution = 224; // CLIP-ViT-B/32 输入分辨率
		Request.Views = { ECanonicalView::Iso };
		Request.bComputeSilhouette = false;
		Request.PhaseT = Phase.PhaseT;
		Request.PhaseId = Phase.PhaseId;
		Request.AssetClassHint = ClassHint;

		TArray<FCanonicalRenderResult> Results;
		if (!Renderer.RenderCanonical(Request, Results) || Results.Num() != 1)
		{
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("AssetVisualSemanticIndexer: 渲染 phase %u (t=%.3f) 失败 (%s)"),
				static_cast<uint32>(Phase.PhaseId), Phase.PhaseT, *AssetData.PackageName.ToString());
			return false;
		}

		TArray<float> Embedding;
		if (!Provider->Encode(Results[0].ColorImage, Results[0].SilhouetteImage, Embedding))
		{
			// Encode 失败：commandlet 会把 mesh 重投 OfflineOnly 队列，等下次跑或下次模型就位。
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
		Entry.EmbeddingDtype = 1; // FP16 落盘
		Entry.PhaseId = Phase.PhaseId;
		Entry.PhaseT = Phase.PhaseT;
		Entry.PhaseLabel = Phase.Label;

		Entry.EmbeddingBytes.SetNumUninitialized(Embedding.Num() * sizeof(uint16));
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
				Dst[Index] = static_cast<uint16>(Sign);
			}
			else if (Exp >= 31)
			{
				Dst[Index] = static_cast<uint16>(Sign | 0x7c00);
			}
			else
			{
				Dst[Index] = static_cast<uint16>(Sign | (static_cast<uint32>(Exp) << 10) | (Mant >> 13));
			}
		}

		Entries.Add(MoveTemp(Entry));
		// semantic 不持久化 PNG；preview_view 路径在 Materialize 阶段从 geometric cohort 同 PhaseId 行复用。
		EmptyPngs.Add(TArray<uint8>());
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	AssetVisualArtifactSerializer::SerializePayload(Entries, EmptyPngs, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FAssetVisualSemanticIndexer::MaterializeArtifact(
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
	if (Entries.Num() == 0)
	{
		return false;
	}

	// preview_view 复用 geometric cohort 已落盘的 PNG（geometric 与 semantic 走同一份 GetPhasesForAsset，
	// 同 phase_id 共用同一张 PNG）。这里按 PhaseId 在 geometric cohort 行里查匹配条目；
	// 找不到匹配就 fallback 到任意可见行的 PreviewViewPath。
	const TArray<FIndexedAssetVisualEntry> GeometricRows = DB.GetAssetVisualEntriesForAsset(TEXT("AssetVisualGeometric"), AssetId);
	auto FindGeoPreviewForPhase = [&GeometricRows](uint8 PhaseId) -> FString
	{
		for (const FIndexedAssetVisualEntry& Geo : GeometricRows)
		{
			if (Geo.PhaseId == PhaseId)
			{
				return Geo.PreviewViewPath;
			}
		}
		return GeometricRows.Num() > 0 ? GeometricRows[0].PreviewViewPath : FString();
	};

	bool bAllOk = true;
	for (FIndexedAssetVisualEntry& Entry : Entries)
	{
		Entry.AssetId = AssetId;
		Entry.PreviewViewPath = FindGeoPreviewForPhase(Entry.PhaseId);
		if (DB.InsertAssetVisualEntry(TEXT("AssetVisualSemantic"), Entry) <= 0)
		{
			bAllOk = false;
		}
	}
	return bAllOk;
}

bool FAssetVisualSemanticIndexer::MaterializeArtifactToShadow(
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
	if (Entries.Num() == 0)
	{
		return false;
	}

	const TArray<FIndexedAssetVisualEntry> GeometricRows = DB.GetAssetVisualEntriesForAsset(TEXT("AssetVisualGeometric"), AssetId);
	auto FindGeoPreviewForPhase = [&GeometricRows](uint8 PhaseId) -> FString
	{
		for (const FIndexedAssetVisualEntry& Geo : GeometricRows)
		{
			if (Geo.PhaseId == PhaseId)
			{
				return Geo.PreviewViewPath;
			}
		}
		return GeometricRows.Num() > 0 ? GeometricRows[0].PreviewViewPath : FString();
	};

	TArray<FMonolithShadowIndexedAssetVisualEntry> ShadowEntries;
	ShadowEntries.Reserve(Entries.Num());
	for (FIndexedAssetVisualEntry& Entry : Entries)
	{
		Entry.AssetId = AssetId;
		Entry.PreviewViewPath = FindGeoPreviewForPhase(Entry.PhaseId);

		FMonolithShadowIndexedAssetVisualEntry ShadowEntry;
		ShadowEntry.Entry = Entry;
		ShadowEntry.RowHash = ComputeAssetVisualRowHash(Entry);
		ShadowEntries.Add(MoveTemp(ShadowEntry));
	}

	return DB.ReplaceShadowAssetVisualEntriesForAsset(
		TEXT("AssetVisualSemantic"),
		CohortName,
		AssetId,
		ShadowEntries);
}
