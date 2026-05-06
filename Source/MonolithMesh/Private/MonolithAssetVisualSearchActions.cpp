#include "MonolithAssetVisualSearchActions.h"
#include "MonolithParamSchema.h"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "AssetVisualEmbeddingProvider.h"
#include "AssetVisualEntry.h"
#include "AssetVisualSharding.h"
#include "AssetVisualShardedRetriever.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexLog.h"
#include "MonolithIndexSubsystem.h"
#include "Modules/ModuleManager.h"

/*
 * Search action 实现要点：
 *  - 全程在 BackgroundCpuPool 跑（不能 IsInGameThread() 断言；编辑器对象访问受限）；
 *  - 视觉 cohort 当前 stale / provider 不可用时优雅降级（顶层 cohort_stale 列表）；
 *  - 候选融合：geometric + semantic 各自 top-K，按 fused score 重排取最终 top-K。
 */
namespace MonolithAssetVisualSearchInternal
{
	/** 默认 / 最大 top-K（与 spec 一致）。 */
	static constexpr int32 DefaultTopK = 10;
	static constexpr int32 MaxTopK = 100;
	/** 图片输入长边上限。 */
	static constexpr int32 MaxImageLongSide = 4096;
	/** 超过 MaxImageLongSide 时降采样到的目标长边。 */
	static constexpr int32 DownsampleLongSide = 1024;
	/** image_base64 字节上限。 */
	static constexpr int32 MaxBase64DecodedBytes = 8 * 1024 * 1024;

	/** 把 path 沙箱到项目 Saved/ 或 Intermediate/Capture/。 */
	static bool IsImagePathInsideSandbox(const FString& AbsolutePath)
	{
		const FString Saved = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		const FString Intermediate = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("Capture")));
		return AbsolutePath.StartsWith(Saved) || AbsolutePath.StartsWith(Intermediate);
	}

	/** 嗅探 PNG/JPEG/WebP；其他格式返回 EImageFormat::Invalid。 */
	static EImageFormat DetectImageFormat(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() >= 8 && Bytes[0] == 0x89 && Bytes[1] == 0x50 && Bytes[2] == 0x4E && Bytes[3] == 0x47)
		{
			return EImageFormat::PNG;
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xFF && Bytes[1] == 0xD8 && Bytes[2] == 0xFF)
		{
			return EImageFormat::JPEG;
		}
		if (Bytes.Num() >= 12 && Bytes[8] == 'W' && Bytes[9] == 'E' && Bytes[10] == 'B' && Bytes[11] == 'P')
		{
			// 'RIFF' .... 'WEBP'
			return EImageFormat::PNG; // ImageWrapper 不区分 WebP 时回退 PNG 解码栈
		}
		return EImageFormat::Invalid;
	}

	/** 把 PNG/JPEG/WebP 字节流解成 BGRA8 sRGB FImage；超过长边上限时降采样。 */
	static bool DecodeImageToBgra8(const TArray<uint8>& Bytes, FImage& OutImage, FString& OutError)
	{
		const EImageFormat Format = DetectImageFormat(Bytes);
		if (Format == EImageFormat::Invalid)
		{
			OutError = TEXT("不支持的图像格式（仅支持 PNG / JPEG / WebP）");
			return false;
		}

		IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = WrapperModule.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			OutError = TEXT("ImageWrapper 解码失败");
			return false;
		}

		TArray<uint8> Raw;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			OutError = TEXT("ImageWrapper GetRaw 失败");
			return false;
		}
		const int32 SrcW = Wrapper->GetWidth();
		const int32 SrcH = Wrapper->GetHeight();
		if (FMath::Max(SrcW, SrcH) > MaxImageLongSide)
		{
			OutError = FString::Printf(TEXT("图像长边 %d 超过上限 %d"), FMath::Max(SrcW, SrcH), MaxImageLongSide);
			return false;
		}

		FImage Decoded;
		Decoded.Init(SrcW, SrcH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(Decoded.RawData.GetData(), Raw.GetData(), Raw.Num());

		// 长边超过 DownsampleLongSide 时统一降采样，让 embedding 推理时间可控。
		const int32 LongSide = FMath::Max(SrcW, SrcH);
		if (LongSide > DownsampleLongSide)
		{
			const float Scale = static_cast<float>(DownsampleLongSide) / static_cast<float>(LongSide);
			const int32 DstW = FMath::Max(1, static_cast<int32>(SrcW * Scale));
			const int32 DstH = FMath::Max(1, static_cast<int32>(SrcH * Scale));
			OutImage.Init(DstW, DstH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			const FColor* Src = reinterpret_cast<const FColor*>(Decoded.RawData.GetData());
			FColor* Dst = reinterpret_cast<FColor*>(OutImage.RawData.GetData());
			// 简单 nearest 降采样；查询路径不需要严苛质量。
			for (int32 Y = 0; Y < DstH; ++Y)
			{
				const int32 SrcY = (Y * SrcH) / DstH;
				for (int32 X = 0; X < DstW; ++X)
				{
					const int32 SrcX = (X * SrcW) / DstW;
					Dst[Y * DstW + X] = Src[SrcY * SrcW + SrcX];
				}
			}
		}
		else
		{
			OutImage = MoveTemp(Decoded);
		}
		return true;
	}

	/** 解析 bbox 参数；返回 false 表示参数无效。 */
	static bool ParseBbox(const TSharedPtr<FJsonObject>& Params, int32 ImageW, int32 ImageH, FIntRect& OutBbox)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("bbox"), Arr) || Arr->Num() != 4)
		{
			return false;
		}
		const int32 X = static_cast<int32>((*Arr)[0]->AsNumber());
		const int32 Y = static_cast<int32>((*Arr)[1]->AsNumber());
		const int32 W = static_cast<int32>((*Arr)[2]->AsNumber());
		const int32 H = static_cast<int32>((*Arr)[3]->AsNumber());
		if (W <= 0 || H <= 0 || X < 0 || Y < 0 || X + W > ImageW || Y + H > ImageH)
		{
			return false;
		}
		OutBbox = FIntRect(X, Y, X + W, Y + H);
		return true;
	}

	/** 把图像按 bbox 裁剪。 */
	static void CropImageInPlace(FImage& Image, const FIntRect& Bbox)
	{
		const int32 SrcW = Image.SizeX;
		const int32 SrcH = Image.SizeY;
		(void)SrcH;
		const int32 NewW = Bbox.Width();
		const int32 NewH = Bbox.Height();
		FImage Out;
		Out.Init(NewW, NewH, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const FColor* Src = reinterpret_cast<const FColor*>(Image.RawData.GetData());
		FColor* Dst = reinterpret_cast<FColor*>(Out.RawData.GetData());
		for (int32 Y = 0; Y < NewH; ++Y)
		{
			for (int32 X = 0; X < NewW; ++X)
			{
				Dst[Y * NewW + X] = Src[(Bbox.Min.Y + Y) * SrcW + (Bbox.Min.X + X)];
			}
		}
		Image = MoveTemp(Out);
	}

	/** 把 cohort 全部 mesh 行加载成 retriever 期望的 ShardEmbeddings 列表。 */
	static void LoadCohortShardEmbeddings(
		FMonolithIndexDatabase& DB,
		const FString& CohortName,
		TArray<FAssetVisualShardEmbeddings>& OutShards)
	{
		OutShards.Reset();

		const TArray<FIndexedAssetVisualEntry> AllEntries = DB.GetAssetVisualEntries(CohortName, FString());
		if (AllEntries.Num() == 0)
		{
			return;
		}

		// 按 ShardId 分组。
		TMap<FString, TArray<const FIndexedAssetVisualEntry*>> ByShard;
		for (const FIndexedAssetVisualEntry& Entry : AllEntries)
		{
			ByShard.FindOrAdd(Entry.ShardId).Add(&Entry);
		}

		OutShards.Reserve(ByShard.Num());
		for (TPair<FString, TArray<const FIndexedAssetVisualEntry*>>& Pair : ByShard)
		{
			FAssetVisualShardEmbeddings Shard;
			Shard.ShardId = Pair.Key;
			Shard.bL2Normalized = true;

			Shard.AssetPaths.Reserve(Pair.Value.Num());
			Shard.RowPhaseIds.Reserve(Pair.Value.Num());
			if (Pair.Value.Num() > 0)
			{
				Shard.EmbeddingDim = Pair.Value[0]->EmbeddingDim;
			}

			for (const FIndexedAssetVisualEntry* Entry : Pair.Value)
			{
				if (Entry->EmbeddingDim != Shard.EmbeddingDim)
				{
					// shard 内不一致维度直接跳过：cohort 升级期间可能短暂出现混合，
					// 顶层会通过 cohort_stale 提示调用方。
					continue;
				}
				Shard.AssetPaths.Add(Entry->AssetPath);
				Shard.RowPhaseIds.Add(Entry->PhaseId);
				if (Entry->EmbeddingDtype == 0)
				{
					// FP32 直接 append。
					const float* SrcF32 = reinterpret_cast<const float*>(Entry->EmbeddingBytes.GetData());
					Shard.Vectors.Append(SrcF32, Entry->EmbeddingDim);
				}
				else
				{
					// FP16 → FP32 widen（与 reducer 同一份算法）。
					const uint16* SrcF16 = reinterpret_cast<const uint16*>(Entry->EmbeddingBytes.GetData());
					for (int32 D = 0; D < Entry->EmbeddingDim; ++D)
					{
						const uint16 H = SrcF16[D];
						const uint32 Sign = (H >> 15) & 0x1;
						const uint32 Exp = (H >> 10) & 0x1f;
						const uint32 Mant = H & 0x3ff;
						uint32 F = 0;
						if (Exp == 0)
						{
							F = (Mant == 0) ? (Sign << 31) : 0;
						}
						else if (Exp == 31)
						{
							F = (Sign << 31) | 0x7f800000 | (Mant << 13);
						}
						else
						{
							F = (Sign << 31) | (static_cast<uint32>(Exp - 15 + 127) << 23) | (Mant << 13);
						}
						float V = 0.0f;
						FMemory::Memcpy(&V, &F, sizeof(float));
						Shard.Vectors.Add(V);
					}
				}
			}

			if (Shard.AssetPaths.Num() > 0)
			{
				OutShards.Add(MoveTemp(Shard));
			}
		}
	}

	/** 把命中转 JSON 对象。 */
	static TSharedPtr<FJsonObject> BuildHitJson(
		const FAssetVisualRetrieverHit& VisualHit,
		const FString& ProviderId,
		const FString& PreviewViewPath,
		const float TotalScore,
		const float RerankScore,
		const float CategoryMatch,
		const float SizeMatch,
		const bool bStale,
		const uint8 BestPhaseId,
		const float BestPhaseT,
		const FString& BestPhaseLabel)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), VisualHit.AssetPath);
		Obj->SetStringField(TEXT("preview_view"), PreviewViewPath);
		Obj->SetNumberField(TEXT("total_score"), TotalScore);
		Obj->SetNumberField(TEXT("rerank_score"), RerankScore);
		Obj->SetBoolField(TEXT("stale"), bStale);

		TSharedPtr<FJsonObject> ProviderBreakdown = MakeShared<FJsonObject>();
		ProviderBreakdown->SetStringField(TEXT("provider_id"), ProviderId);
		ProviderBreakdown->SetNumberField(TEXT("visual_score"), VisualHit.Score);
		ProviderBreakdown->SetStringField(TEXT("shard_id"), VisualHit.ShardId);
		Obj->SetObjectField(TEXT("provider_breakdown"), ProviderBreakdown);

		Obj->SetNumberField(TEXT("category_match"), CategoryMatch);
		Obj->SetNumberField(TEXT("size_match"), SizeMatch);

		// Multi-phase 命中信息：让调用方知道这次最优分来自哪个 phase（"早 / 中 / 晚 / 0.5s 仿真" 等）。
		// 单 phase 资产 BestPhaseId=0, BestPhaseT=0, BestPhaseLabel=""，仍然正确。
		Obj->SetNumberField(TEXT("best_phase_id"), static_cast<int32>(BestPhaseId));
		Obj->SetNumberField(TEXT("best_phase_t"), BestPhaseT);
		Obj->SetStringField(TEXT("best_phase_label"), BestPhaseLabel);
		return Obj;
	}
}

void FMonolithAssetVisualSearchActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("search_assets_by_image"),
		TEXT("Search assets (StaticMesh / SkeletalMesh / Material / WidgetBlueprint) visually similar to a given image, fusing geometric + semantic cohorts"),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetVisualSearchActions::HandleSearchAssetsByImage),
		FParamSchemaBuilder()
			.Optional(TEXT("image_path"), TEXT("string"), TEXT("项目内 Saved/ 或 Intermediate/Capture/ 下的图片路径"))
			.Optional(TEXT("image_base64"), TEXT("string"), TEXT("Base64 编码的 PNG/JPEG/WebP 字节"))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("auto | geometric | semantic | both"), TEXT("auto"))
			.Optional(TEXT("top_k"), TEXT("integer"), TEXT("Top-K 候选数；默认 10，clamp 到 [1,100]"), TEXT("10"))
			.Optional(TEXT("bbox"), TEXT("array"), TEXT("[x,y,w,h] 像素坐标，原点左上"))
			.Optional(TEXT("category_hint"), TEXT("string"), TEXT("rerank 加权类别提示"))
			.Optional(TEXT("size_hint"), TEXT("array"), TEXT("[x,y,z] 期望尺寸（cm），rerank 加权"))
			.Build(),
		EMonolithActionExecutionPolicy::BackgroundThread);
}

FMonolithActionResult FMonolithAssetVisualSearchActions::HandleSearchAssetsByImage(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithAssetVisualSearchInternal;

	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Params is null"));
	}

	const double StartSeconds = FPlatformTime::Seconds();

	// 1) 解析图片输入：image_path 或 image_base64 二选一。
	TArray<uint8> ImageBytes;
	if (Params->HasTypedField<EJson::String>(TEXT("image_path")))
	{
		const FString InputPath = Params->GetStringField(TEXT("image_path"));
		if (InputPath.Contains(TEXT("..")))
		{
			return FMonolithActionResult::Error(TEXT("image_path 不能包含 .. 路径段"));
		}
		const FString Absolute = FPaths::ConvertRelativePathToFull(InputPath);
		if (!IsImagePathInsideSandbox(Absolute))
		{
			return FMonolithActionResult::Error(TEXT("image_path 必须在 Saved/ 或 Intermediate/Capture/ 下"));
		}
		if (!FFileHelper::LoadFileToArray(ImageBytes, *Absolute))
		{
			return FMonolithActionResult::Error(TEXT("加载 image_path 文件失败"));
		}
	}
	else if (Params->HasTypedField<EJson::String>(TEXT("image_base64")))
	{
		const FString B64 = Params->GetStringField(TEXT("image_base64"));
		if (!FBase64::Decode(B64, ImageBytes))
		{
			return FMonolithActionResult::Error(TEXT("image_base64 解码失败"));
		}
		if (ImageBytes.Num() > MaxBase64DecodedBytes)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("image_base64 解码后字节数 %d 超过上限 %d"),
				ImageBytes.Num(), MaxBase64DecodedBytes));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("必须提供 image_path 或 image_base64 之一"));
	}

	FImage Image;
	{
		FString DecodeError;
		if (!DecodeImageToBgra8(ImageBytes, Image, DecodeError))
		{
			return FMonolithActionResult::Error(DecodeError);
		}
	}

	// 2) 可选 bbox 裁剪。
	FIntRect Bbox;
	if (ParseBbox(Params, Image.SizeX, Image.SizeY, Bbox))
	{
		CropImageInPlace(Image, Bbox);
	}

	// 3) provider 选择：auto / geometric / semantic / both
	FString ProviderArg = TEXT("auto");
	if (Params->HasTypedField<EJson::String>(TEXT("provider")))
	{
		ProviderArg = Params->GetStringField(TEXT("provider")).ToLower();
	}
	const bool bWantGeometric = ProviderArg == TEXT("auto") || ProviderArg == TEXT("geometric") || ProviderArg == TEXT("both");
	const bool bWantSemantic = ProviderArg == TEXT("auto") || ProviderArg == TEXT("semantic") || ProviderArg == TEXT("both");

	// 4) top_k clamp。
	int32 TopK = DefaultTopK;
	if (Params->HasTypedField<EJson::Number>(TEXT("top_k")))
	{
		TopK = static_cast<int32>(Params->GetNumberField(TEXT("top_k")));
	}
	TopK = FMath::Clamp(TopK, 1, MaxTopK);

	// 5) 拿到 subsystem 与 DB 引用。
	UMonolithIndexSubsystem* IndexSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	if (!IndexSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithIndexSubsystem unavailable"));
	}

	// 6) 跑 query：geometric + semantic 并行
	struct FCohortResult
	{
		FString CohortName;
		FString ProviderId;
		bool bUsed = false;
		TArray<FAssetVisualRetrieverHit> Hits;
	};
	FCohortResult Geometric;
	FCohortResult Semantic;
	Geometric.CohortName = TEXT("AssetVisualGeometric");
	Geometric.ProviderId = TEXT("geometric_v1");
	Semantic.CohortName = TEXT("AssetVisualSemantic");
	Semantic.ProviderId = TEXT("clip_vit_b32_v1");

	TArray<FString> CohortStaleList;

	auto ExecCohort = [&](FCohortResult& Cohort, const TFunction<bool(TArray<float>&)>& EncodeFn)
	{
		const FMonolithActionResult Result = IndexSubsystem->RunReadDatabaseAction([&](FMonolithIndexDatabase& DB)
		{
			TArray<FAssetVisualShardEmbeddings> Shards;
			LoadCohortShardEmbeddings(DB, Cohort.CohortName, Shards);
			if (Shards.Num() == 0)
			{
				CohortStaleList.Add(Cohort.CohortName);
				return FMonolithActionResult::Success(MakeShared<FJsonObject>());
			}

			TArray<float> QueryVec;
			if (!EncodeFn(QueryVec) || QueryVec.Num() == 0)
			{
				CohortStaleList.Add(Cohort.CohortName);
				return FMonolithActionResult::Success(MakeShared<FJsonObject>());
			}

			FAssetVisualRetrieverQuery Q;
			Q.QueryVector = QueryVec;
			Q.TopK = TopK;
			FAssetVisualShardedRetriever Retriever;
			Retriever.QueryAcrossShards(Shards, Q, Cohort.Hits);
			Cohort.bUsed = true;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		});
		(void)Result;
	};

	if (bWantGeometric)
	{
		ExecCohort(Geometric, [&](TArray<float>& OutVec)
		{
			// 索引/查询走完全相同的 Encode：拿 registry 里的 geometric provider 直接调用。
			// silhouette 由 provider helper 从 color 推导（"非黑像素=前景"），
			// 与 IAssetCanonicalRenderer 内部阈值完全一致，保证 query/index 同图同向量。
			const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
				FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(FName(TEXT("geometric_v1")));
			if (!Provider.IsValid() || !Provider->IsAvailable())
			{
				return false;
			}
			FImage Silhouette;
			DeriveAssetVisualSilhouetteFromColor(Image, Silhouette);
			return Provider->Encode(Image, Silhouette, OutVec);
		});
	}

	if (bWantSemantic)
	{
		ExecCohort(Semantic, [&](TArray<float>& OutVec)
		{
			const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
				FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(FName(TEXT("clip_vit_b32_v1")));
			if (!Provider.IsValid() || !Provider->IsAvailable())
			{
				return false;
			}
			FImage EmptySilhouette;
			return Provider->Encode(Image, EmptySilhouette, OutVec);
		});
	}

	// 7) 融合 + dedup：每 (asset_path, provider) 组取最高分 phase。
	//
	//    Multi-phase 资产同 asset_path 在 cohort 里会出现多行（每 phase 一行 embedding），
	//    retriever top-K 可能把同一 asset 的多个 phase 都返回；如果不 dedup，会浪费 result list 的槽位。
	//    每 (asset_path, provider) 只保留最高分那一行 + 它对应的 PhaseId，给调用方报 best_phase_*。
	struct FCandidate
	{
		FString AssetPath;
		float VisualScore = 0.0f;
		FString ProviderId;
		FString ShardId;
		uint8 BestPhaseId = 0;
	};

	auto MergeBestPhase = [](TMap<FString, FCandidate>& ByAsset, const FAssetVisualRetrieverHit& Hit, const FString& ProviderId)
	{
		FCandidate* Existing = ByAsset.Find(Hit.AssetPath);
		if (!Existing)
		{
			FCandidate C;
			C.AssetPath = Hit.AssetPath;
			C.VisualScore = Hit.Score;
			C.ProviderId = ProviderId;
			C.ShardId = Hit.ShardId;
			C.BestPhaseId = Hit.PhaseId;
			ByAsset.Add(Hit.AssetPath, MoveTemp(C));
			return;
		}
		if (Hit.Score > Existing->VisualScore)
		{
			Existing->VisualScore = Hit.Score;
			Existing->ShardId = Hit.ShardId;
			Existing->BestPhaseId = Hit.PhaseId;
		}
	};

	TMap<FString, FCandidate> GeoByAsset;
	for (const FAssetVisualRetrieverHit& H : Geometric.Hits)
	{
		MergeBestPhase(GeoByAsset, H, Geometric.ProviderId);
	}
	TMap<FString, FCandidate> SemByAsset;
	for (const FAssetVisualRetrieverHit& H : Semantic.Hits)
	{
		MergeBestPhase(SemByAsset, H, Semantic.ProviderId);
	}

	TArray<FCandidate> Candidates;
	Candidates.Reserve(GeoByAsset.Num() + SemByAsset.Num());
	for (TPair<FString, FCandidate>& Pair : GeoByAsset)
	{
		Candidates.Add(MoveTemp(Pair.Value));
	}
	for (TPair<FString, FCandidate>& Pair : SemByAsset)
	{
		Candidates.Add(MoveTemp(Pair.Value));
	}

	// rerank：结合 MeshCatalog 的 size / category。
	const FString CategoryHint = Params->HasTypedField<EJson::String>(TEXT("category_hint"))
		? Params->GetStringField(TEXT("category_hint"))
		: FString();
	TArray<TSharedPtr<FJsonValue>> SizeHintArr;
	if (Params->HasTypedField<EJson::Array>(TEXT("size_hint")))
	{
		SizeHintArr = Params->GetArrayField(TEXT("size_hint"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	{
		const FMonolithActionResult RerankResult = IndexSubsystem->RunReadDatabaseAction([&](FMonolithIndexDatabase& DB)
		{
			for (const FCandidate& C : Candidates)
			{
				// 拿 mesh catalog 行做 rerank 加权。
				const TArray<FIndexedMeshCatalogEntry> CatalogRows = DB.GetMeshCatalogEntries(C.AssetPath);
				float CategoryMatch = 0.0f;
				float SizeMatch = 0.0f;
				FString PreviewViewPath;
				const bool bRowHit = (CatalogRows.Num() > 0);
				if (bRowHit)
				{
					const FIndexedMeshCatalogEntry& Row = CatalogRows[0];
					if (!CategoryHint.IsEmpty() && !Row.Category.IsEmpty())
					{
						CategoryMatch = Row.Category.Contains(CategoryHint) ? 1.0f : 0.0f;
					}
					if (SizeHintArr.Num() == 3)
					{
						const float HX = static_cast<float>(SizeHintArr[0]->AsNumber());
						const float HY = static_cast<float>(SizeHintArr[1]->AsNumber());
						const float HZ = static_cast<float>(SizeHintArr[2]->AsNumber());
						const float DX = FMath::Abs(static_cast<float>(Row.BoundsX) - HX);
						const float DY = FMath::Abs(static_cast<float>(Row.BoundsY) - HY);
						const float DZ = FMath::Abs(static_cast<float>(Row.BoundsZ) - HZ);
						// 简单加权：偏差 / hint 总尺寸 → 越小越像。
						const float HSum = HX + HY + HZ + 1e-3f;
						const float Diff = (DX + DY + DZ) / HSum;
						SizeMatch = FMath::Clamp(1.0f - Diff, 0.0f, 1.0f);
					}
				}

				// 从 visual cohort 行里拿 best phase 对应的 preview_view 路径 + phase 元信息。
				// 多 phase 资产命中的 PhaseId 决定了哪张 PNG 是最相似的；优先匹配 (asset_path, phase_id)。
				// fallback：找不到精确匹配就用任意可见 PhaseId=0 行（兼容老数据）。
				float BestPhaseT = 0.0f;
				FString BestPhaseLabel;
				const TArray<FIndexedAssetVisualEntry> GeoRows = DB.GetAssetVisualEntries(TEXT("AssetVisualGeometric"), FString());
				bool bFoundPhase = false;
				for (const FIndexedAssetVisualEntry& Row : GeoRows)
				{
					if (Row.AssetPath == C.AssetPath && Row.PhaseId == C.BestPhaseId)
					{
						PreviewViewPath = Row.PreviewViewPath;
						BestPhaseT = Row.PhaseT;
						BestPhaseLabel = Row.PhaseLabel;
						bFoundPhase = true;
						break;
					}
				}
				if (!bFoundPhase)
				{
					for (const FIndexedAssetVisualEntry& Row : GeoRows)
					{
						if (Row.AssetPath == C.AssetPath)
						{
							PreviewViewPath = Row.PreviewViewPath;
							BestPhaseT = Row.PhaseT;
							BestPhaseLabel = Row.PhaseLabel;
							break;
						}
					}
				}

				const float RerankScore = 0.5f * CategoryMatch + 0.5f * SizeMatch;
				const float TotalScore = 0.7f * C.VisualScore + 0.3f * RerankScore;
				const bool bStaleEntry = (CohortStaleList.Find(C.ProviderId == TEXT("geometric_v1") ? FString(TEXT("AssetVisualGeometric")) : FString(TEXT("AssetVisualSemantic"))) != INDEX_NONE);

				FAssetVisualRetrieverHit VisualHit;
				VisualHit.AssetPath = C.AssetPath;
				VisualHit.Score = C.VisualScore;
				VisualHit.ShardId = C.ShardId;
				VisualHit.PhaseId = C.BestPhaseId;
				ResultsJson.Add(MakeShared<FJsonValueObject>(
					BuildHitJson(VisualHit, C.ProviderId, PreviewViewPath, TotalScore, RerankScore, CategoryMatch, SizeMatch, bStaleEntry,
						C.BestPhaseId, BestPhaseT, BestPhaseLabel)));
			}
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		});
		(void)RerankResult;
	}

	// 按 total_score 排序，截 top-K。
	ResultsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const double SA = A->AsObject()->GetNumberField(TEXT("total_score"));
		const double SB = B->AsObject()->GetNumberField(TEXT("total_score"));
		return SA > SB;
	});
	if (ResultsJson.Num() > TopK)
	{
		ResultsJson.SetNum(TopK, EAllowShrinking::No);
	}

	// 装顶层响应。
	TSharedPtr<FJsonObject> TopLevel = MakeShared<FJsonObject>();
	TopLevel->SetArrayField(TEXT("results"), ResultsJson);
	const TSharedPtr<FJsonObject> Stats = IndexSubsystem->GetStats();
	const bool bIndexingInProgress = Stats.IsValid() && Stats->GetBoolField(TEXT("indexing_in_progress"));
	TopLevel->SetBoolField(TEXT("indexing_in_progress"), bIndexingInProgress);

	bool bAnyStale = (CohortStaleList.Num() > 0);
	for (const TSharedPtr<FJsonValue>& Hit : ResultsJson)
	{
		if (Hit->AsObject()->GetBoolField(TEXT("stale")))
		{
			bAnyStale = true;
			break;
		}
	}
	TopLevel->SetBoolField(TEXT("stale"), bAnyStale);

	TArray<TSharedPtr<FJsonValue>> CohortStaleJsonArr;
	for (const FString& C : CohortStaleList) CohortStaleJsonArr.Add(MakeShared<FJsonValueString>(C));
	TopLevel->SetArrayField(TEXT("cohort_stale"), CohortStaleJsonArr);

	// 顶层 provider 元信息。
	TSharedPtr<FJsonObject> ProvidersJson = MakeShared<FJsonObject>();
	if (Geometric.bUsed)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("embedding_provider"), Geometric.ProviderId);
		Obj->SetNumberField(TEXT("embedding_version"), 1);
		ProvidersJson->SetObjectField(TEXT("geometric"), Obj);
	}
	if (Semantic.bUsed)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("embedding_provider"), Semantic.ProviderId);
		const TSharedPtr<IAssetVisualEmbeddingProvider> Provider =
			FAssetVisualEmbeddingProviderRegistry::Get().FindProvider(FName(*Semantic.ProviderId));
		Obj->SetNumberField(TEXT("embedding_version"), Provider.IsValid() ? Provider->GetProviderInfo().ProviderVersion : 1);
		ProvidersJson->SetObjectField(TEXT("semantic"), Obj);
	}
	TopLevel->SetObjectField(TEXT("providers"), ProvidersJson);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TopLevel->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);

	return FMonolithActionResult::Success(TopLevel);
}
