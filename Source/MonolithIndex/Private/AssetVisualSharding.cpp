#include "AssetVisualSharding.h"

#include "MonolithIndexLog.h"

/*
 * Sharding 实现的核心是“稳定 + 单段递归再拆”。
 *
 * 不引入任何随机源、不依赖 mesh 内容、不依赖系统时间——同一份输入同一份策略，
 * 在编辑器、Horde agent、自动化测试里都得到 bit-identical 的映射。
 *
 * 实现刻意保持线性 O(N * PrefixDepth)，方便 50 万规模下一次 pass 算完不再做二次排序。
 */
namespace AssetVisualShardingInternal
{
	/** path 段切分：把 `/A/B/C/...` 拆成 [A, B, C, ...]；空段忽略。 */
	static void SplitPathSegments(const FString& AssetPath, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();

		// 资产路径形如 `/Game/Buildings/Houses/SM_House.SM_House`，
		// 我们只关心前面的目录段（不含 `.短名`）。
		FString DirectoryPart = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (DirectoryPart.FindLastChar(TEXT('.'), DotIndex))
		{
			DirectoryPart.LeftInline(DotIndex);
		}

		DirectoryPart.ParseIntoArray(OutSegments, TEXT("/"), /*InCullEmpty=*/true);

		// 最后一段是 mesh 短名（例如 SM_House），不应该影响 shard 划分；剔除。
		if (OutSegments.Num() > 0)
		{
			OutSegments.Pop();
		}
	}

	/** 按截定 PrefixDepth 段拼接稳定 ShardId；不足段用 `_` 占位保持对齐。 */
	static FString BuildShardId(const TArray<FString>& Segments, const int32 PrefixDepth)
	{
		FString ShardId;
		ShardId.Reserve(64);

		for (int32 SegmentIndex = 0; SegmentIndex < PrefixDepth; ++SegmentIndex)
		{
			if (SegmentIndex > 0)
			{
				ShardId.AppendChar(TEXT('.'));
			}

			if (Segments.IsValidIndex(SegmentIndex))
			{
				// 替换段内可能出现的不安全字符，避免后续把 ShardId 拼进 DDC bucket / 文件名时翻车。
				FString Safe = Segments[SegmentIndex];
				Safe.ReplaceInline(TEXT(":"), TEXT("_"));
				Safe.ReplaceInline(TEXT(" "), TEXT("_"));
				Safe.ReplaceInline(TEXT("\\"), TEXT("_"));
				ShardId.Append(Safe);
			}
			else
			{
				ShardId.AppendChar(TEXT('_'));
			}
		}

		// 完全没有任何段的情况（理论上不可能）兜底为固定值。
		if (ShardId.IsEmpty())
		{
			ShardId = TEXT("_");
		}

		return ShardId;
	}
}

FAssetVisualShardKey ComputeAssetVisualShardKey(const FString& AssetPath, const int32 PrefixDepth)
{
	const int32 ClampedDepth = FMath::Clamp(PrefixDepth, 1, 16);

	TArray<FString> Segments;
	AssetVisualShardingInternal::SplitPathSegments(AssetPath, Segments);

	FAssetVisualShardKey Key;
	Key.PrefixDepth = ClampedDepth;
	Key.ShardId = AssetVisualShardingInternal::BuildShardId(Segments, ClampedDepth);
	return Key;
}

TMap<FString, FAssetVisualShardKey> AssignAssetVisualShardsForCohort(
	const TArray<FString>& AssetPaths,
	const FAssetVisualShardCapacityPolicy& Policy)
{
	using namespace AssetVisualShardingInternal;

	const int32 InitialDepth = FMath::Clamp(Policy.InitialPrefixDepth, 1, FMath::Max(1, Policy.MaxPrefixDepth));
	const int32 MaxDepth = FMath::Max(InitialDepth, Policy.MaxPrefixDepth);
	const int32 CapacityLimit = FMath::Max(1, Policy.MaxMeshesPerShard);

	// 先把每个资产的 path 段切好，避免后面递归时重复 split。
	TMap<FString, TArray<FString>> SegmentsCache;
	SegmentsCache.Reserve(AssetPaths.Num());
	for (const FString& AssetPath : AssetPaths)
	{
		TArray<FString> Segments;
		SplitPathSegments(AssetPath, Segments);
		SegmentsCache.Emplace(AssetPath, MoveTemp(Segments));
	}

	// 当前每个资产正在尝试的 PrefixDepth；初始化成 InitialDepth。
	TMap<FString, int32> CurrentDepthByAsset;
	CurrentDepthByAsset.Reserve(AssetPaths.Num());
	for (const FString& AssetPath : AssetPaths)
	{
		CurrentDepthByAsset.Add(AssetPath, InitialDepth);
	}

	// 迭代：直到没有任何 shard 仍然超容量，或全部已经达到 MaxPrefixDepth。
	while (true)
	{
		// 按当前深度统计每个 ShardId 包含多少个 asset。
		TMap<FString, int32> CountByShardId;
		CountByShardId.Reserve(AssetPaths.Num());
		TMap<FString, int32> DepthByShardId;
		DepthByShardId.Reserve(AssetPaths.Num());
		for (const FString& AssetPath : AssetPaths)
		{
			const int32 Depth = CurrentDepthByAsset.FindChecked(AssetPath);
			const FString ShardId = BuildShardId(SegmentsCache.FindChecked(AssetPath), Depth);
			++CountByShardId.FindOrAdd(ShardId);
			DepthByShardId.FindOrAdd(ShardId) = Depth;
		}

		// 把所有超容量、且还能再深的 shard 标记为"下一轮要加深"。
		TSet<FString> OverCapacityShards;
		bool bAnyDeepenable = false;
		for (const TPair<FString, int32>& Pair : CountByShardId)
		{
			if (Pair.Value <= CapacityLimit)
			{
				continue;
			}
			const int32 ShardDepth = DepthByShardId.FindChecked(Pair.Key);
			if (ShardDepth >= MaxDepth)
			{
				// 已经到最深一层还超容量：记 warning，承认这个 shard 就这样了。
				UE_LOG(LogMonolithIndex, Warning,
					TEXT("AssetVisual shard '%s' 在最大深度 %d 仍超容量 (%d > %d)，按当前深度落 shard"),
					*Pair.Key, ShardDepth, Pair.Value, CapacityLimit);
				continue;
			}
			OverCapacityShards.Add(Pair.Key);
			bAnyDeepenable = true;
		}

		if (!bAnyDeepenable)
		{
			break;
		}

		// 把超容量 shard 内所有 asset 的当前深度 +1，进入下一轮。
		for (const FString& AssetPath : AssetPaths)
		{
			int32& Depth = CurrentDepthByAsset.FindChecked(AssetPath);
			const FString ShardId = BuildShardId(SegmentsCache.FindChecked(AssetPath), Depth);
			if (OverCapacityShards.Contains(ShardId))
			{
				++Depth;
			}
		}
	}

	// 最后一遍生成最终 shard key 映射。
	TMap<FString, FAssetVisualShardKey> Result;
	Result.Reserve(AssetPaths.Num());
	for (const FString& AssetPath : AssetPaths)
	{
		const int32 Depth = CurrentDepthByAsset.FindChecked(AssetPath);
		FAssetVisualShardKey Key;
		Key.PrefixDepth = Depth;
		Key.ShardId = BuildShardId(SegmentsCache.FindChecked(AssetPath), Depth);
		Result.Emplace(AssetPath, MoveTemp(Key));
	}
	return Result;
}
