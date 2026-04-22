#include "MonolithGlobalArtifactPipeline.h"

#include "MonolithArtifactCache.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexer.h"
#include "MonolithIndexDatabase.h"

namespace MonolithGlobalArtifactPipelineInternal
{
	/*
	 * 全局 reducer 的 cache/build 决策只保留这一份实现：
	 * 1. 先构建稳定 identity；
	 * 2. 再优先查 artifact cache；
	 * 3. miss 时才真正 build artifact；
	 * 4. build 成功后按需要回写 cache。
	 *
	 * 这样 warmup 和编辑器执行链不会再各自复制一套 cache 命中逻辑。
	 */
	static bool ResolveGlobalArtifact(
		IMonolithIndexer& Indexer,
		IMonolithArtifactCache* ArtifactCache,
		const EMonolithArtifactCacheRequestMode RequestMode,
		const bool bWriteCacheOnMiss,
		FMonolithArtifact& OutArtifact,
		bool& bOutUsedCachedArtifact)
	{
		bOutUsedCachedArtifact = false;

		FMonolithArtifactIdentityV1 Identity;
		if (!Indexer.BuildGlobalArtifactIdentity(Identity))
		{
			return false;
		}

		if (ArtifactCache)
		{
			if (const TOptional<FMonolithArtifact> CachedArtifact = ArtifactCache->Get(Identity, RequestMode))
			{
				OutArtifact = CachedArtifact.GetValue();
				bOutUsedCachedArtifact = true;
				return true;
			}
		}

		if (!Indexer.BuildGlobalArtifact(OutArtifact))
		{
			return false;
		}

		// 只有这条唯一出口负责给新 artifact 补上稳定 identity hash。
		OutArtifact.IdentityHash = HashMonolithArtifactIdentity(Identity);
		if (ArtifactCache && bWriteCacheOnMiss)
		{
			ArtifactCache->Put(Identity, OutArtifact, RequestMode);
		}
		return true;
	}
}

namespace MonolithGlobalArtifactPipeline
{
	bool WarmGlobalIndexerArtifact(IMonolithIndexer& Indexer, IMonolithArtifactCache& ArtifactCache)
	{
		FMonolithArtifact Artifact;
		bool bUsedCachedArtifact = false;
		return MonolithGlobalArtifactPipelineInternal::ResolveGlobalArtifact(
			Indexer,
			&ArtifactCache,
			EMonolithArtifactCacheRequestMode::Warmup,
			true,
			Artifact,
			bUsedCachedArtifact);
	}

	bool ExecuteGlobalIndexerArtifact(
		IMonolithIndexer& Indexer,
		IMonolithArtifactCache* ArtifactCache,
		FMonolithIndexDatabase& DB,
		bool& bOutUsedCachedArtifact)
	{
		FMonolithArtifact Artifact;
		if (!MonolithGlobalArtifactPipelineInternal::ResolveGlobalArtifact(
			Indexer,
			ArtifactCache,
			EMonolithArtifactCacheRequestMode::Background,
			true,
			Artifact,
			bOutUsedCachedArtifact))
		{
			return false;
		}

		return Indexer.MaterializeGlobalArtifact(Artifact, DB);
	}
}
