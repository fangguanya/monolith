#pragma once

#include "CoreMinimal.h"

class IMonolithArtifactCache;
class IMonolithIndexer;
class FMonolithIndexDatabase;

/*
 * 这份 helper 把“全局 reducer artifact 主链”收口成唯一实现。
 *
 * 它负责的事情很明确：
 * - 先让 indexer 产出全局 identity；
 * - 再优先尝试 artifact cache；
 * - cache miss 时再现场构建 artifact；
 * - 最后把 artifact materialize 到 SQLite。
 *
 * 这样 commandlet warmup 和编辑器 full index
 * 都能复用同一套流程，而不是各自手写一遍。
 */
namespace MonolithGlobalArtifactPipeline
{
	/** 只做 warmup：构建或复用全局 artifact，但不写 SQLite。 */
	bool WarmGlobalIndexerArtifact(IMonolithIndexer& Indexer, IMonolithArtifactCache& ArtifactCache);

	/** 执行全局 artifact 链路，并把结果 materialize 到数据库。
	 * `bOutUsedCachedArtifact` 会告诉调用方这次到底是不是从 cache 命中的。 */
	bool ExecuteGlobalIndexerArtifact(
		IMonolithIndexer& Indexer,
		IMonolithArtifactCache* ArtifactCache,
		FMonolithIndexDatabase& DB,
		bool& bOutUsedCachedArtifact);
}
