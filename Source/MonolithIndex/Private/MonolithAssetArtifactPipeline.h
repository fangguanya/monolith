#pragma once

#include "CoreMinimal.h"
#include "MonolithArtifactCache.h"

class IMonolithIndexer;
class FMonolithIndexDatabase;
struct FAssetData;

/*
 * 这份 helper 把“单资产 artifact 主链”收口成唯一实现。
 *
 * 它统一负责：
 * 1. 为资产+indexer 构建稳定 identity；
 * 2. 优先尝试 artifact cache；
 * 3. cache miss 时按调用方策略决定是否允许现场 build；
 * 4. 把 artifact materialize 到生产表和可选 shadow 表。
 *
 * 这样编辑器索引、shadow diff 和 warmup commandlet
 * 都能复用同一条 artifact 规则，不再各自复制 cache/build/materialize 逻辑。
 */
namespace MonolithAssetArtifactPipeline
{
	/** 单资产 artifact 执行结果。 */
	enum class EExecuteAssetOutcome : uint8
	{
		/** 这次链路已经完整成功。 */
		Succeeded,
		/** cache miss，但当前调用方不允许现场 build，需要上层决定如何补救。 */
		NeedsLocalBuild,
		/** identity/build/materialize 过程中出现明确失败。 */
		Failed,
	};

	/** 调用方对本次执行的约束。 */
	struct FExecuteAssetOptions
	{
		/** 当前请求模式，决定 cache timeout / breaker 预算。 */
		EMonolithArtifactCacheRequestMode RequestMode = EMonolithArtifactCacheRequestMode::Background;
		/** 是否允许在本进程里现场构建 artifact。 */
		bool bAllowLocalArtifactBuild = false;
		/** 是否把命中的 artifact 回放到正式生产表。 */
		bool bMaterializeProduction = false;
		/** 如果需要写生产表，这里必须提供合法 AssetId。 */
		int64 AssetId = 0;
		/** 如果非空，说明还要把同一份 artifact 镜像到 shadow 表。 */
		FString ShadowCohortName;
	};

	/** 向调用方返回本次执行的事实。 */
	struct FExecuteAssetResult
	{
		/** 当前 artifact 对应的稳定 identity。 */
		TOptional<FMonolithArtifactIdentityV1> Identity;
		/** 这次是否直接命中了 cache。 */
		bool bUsedCachedArtifact = false;
		/** 这次是否在本地现场构建了 artifact。 */
		bool bBuiltArtifactLocally = false;
		/** 是否已经成功 materialize 到正式生产表。 */
		bool bMaterializedProduction = false;
		/** 是否已经成功 materialize 到 shadow 表。 */
		bool bMaterializedShadow = false;
	};

	/** 执行单资产 artifact 链路。
	 *
	 * `LoadedAsset` 是调用方已经准备好的对象，可选；
	 * `LoadAssetForLocalBuild` 只会在 cache miss 且允许现场 build 时才被调用，
	 * 用来把“是否真的要付出加载成本”的决策延后到最后一刻。 */
	EExecuteAssetOutcome ExecuteAssetIndexerArtifact(
		const FAssetData& AssetData,
		UObject* LoadedAsset,
		TUniqueFunction<UObject*()> LoadAssetForLocalBuild,
		IMonolithIndexer& Indexer,
		IMonolithArtifactCache* ArtifactCache,
		FMonolithIndexDatabase* DB,
		const FExecuteAssetOptions& Options,
		FExecuteAssetResult& OutResult);
}
