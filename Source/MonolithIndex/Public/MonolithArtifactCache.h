#pragma once

#include "CoreMinimal.h"
#include "MonolithArtifactTypes.h"

class FQueuedThreadPool;

/*
 * artifact cache 可以理解成“索引结果的仓库管理员”。
 *
 * 它的职责很简单：
 * 1. 先看本地仓库有没有现成结果；
 * 2. 没有的话，再去远端仓库拿；
 * 3. 新生成的结果也要写回仓库。
 *
 * 下面这几个类型就是这个仓库管理员对外公开的接口。
 */

/** 记录 cache 命中和远端熔断状态，方便 status bar / 调试面板展示。 */
struct FMonolithArtifactCacheStats
{
	/** 本地缓存命中了多少次。 */
	uint64 LocalHitCount = 0;
	/** 远端缓存命中了多少次。 */
	uint64 RemoteHitCount = 0;
	/** 远端查询没找到结果多少次。 */
	uint64 RemoteMissCount = 0;
	/** 远端写入成功多少次。 */
	uint64 RemoteWriteOkCount = 0;
	/** 远端写入失败多少次。 */
	uint64 RemoteWriteFailCount = 0;
	/** 成功写到远端的 artifact 编码总字节数。 */
	uint64 RemoteWriteBytes = 0;
	/** 因为原始 payload 超过 16MB 而被跳过共享缓存的次数。 */
	uint64 OversizedArtifactCount = 0;
	/** 还排队等着同步到远端 DDC 的请求数。 */
	int32 PendingRemoteWriteCount = 0;
	/** 当前已经发出去、还没完成的远端写请求数。 */
	int32 InFlightRemoteWriteCount = 0;
	/** 熔断器当前是否把远端暂时关掉了。 */
	bool bRemoteDisabled = false;
	/** 如果远端被关掉，还要等多久才允许再试。 */
	double RemoteBreakerRemainingSeconds = 0.0;
};

/*
 * cache 请求模式用来表达“这次请求到底多怕卡”。
 *
 * 我们只保留三种明确语义：
 * - Interactive: live / 交互链路，远端 get 必须更激进地限时；
 * - Background: 编辑器后台索引链路；
 * - Warmup: 纯预热链路，允许更保守、但仍受统一 timeout/breaker 约束。
 *
 * 这样调用方就不再需要各自发明一套“是前台还是后台”的布尔参数。
 */
enum class EMonolithArtifactCacheRequestMode : uint8
{
	/** 交互或 live 更新链路。 */
	Interactive,
	/** 常规后台索引链路。 */
	Background,
	/** 预热 / commandlet 链路。 */
	Warmup,
};

/** 统一的 artifact cache 抽象接口。 */
class MONOLITHINDEX_API IMonolithArtifactCache
{
public:
	virtual ~IMonolithArtifactCache() = default;

	/** 按 identity 取一份 artifact；如果没命中就返回空。 */
	virtual TOptional<FMonolithArtifact> Get(
		const FMonolithArtifactIdentityV1& Identity,
		EMonolithArtifactCacheRequestMode RequestMode) = 0;
	/** 把一份 artifact 写进缓存；本地失败返回 false。 */
	virtual bool Put(
		const FMonolithArtifactIdentityV1& Identity,
		const FMonolithArtifact& Artifact,
		EMonolithArtifactCacheRequestMode RequestMode) = 0;
	/** 等待已经排队的远端写入尽量收尾。
	 * 返回 true 表示在超时前已经全部排空；
	 * 返回 false 表示还有远端写任务没收完。 */
	virtual bool DrainRemoteWrites(double TimeoutSeconds = -1.0) = 0;
	/** 丢弃还没真正发出去的远端写请求。
	 * 这不会影响已经成功写到本地缓存的数据。 */
	virtual void DiscardPendingRemoteWrites() = 0;
	/** 读取当前统计信息。 */
	virtual FMonolithArtifactCacheStats GetStats() const = 0;
	/** 清空统计计数，但不清空真正缓存内容。 */
	virtual void ResetStats() = 0;
	/** 给 cache 指定一个 IO 线程池，避免占用默认线程。 */
	virtual void SetIoThreadPool(FQueuedThreadPool* InIoThreadPool) = 0;
};

/** 基于 Unreal DDC 的具体实现。 */
class MONOLITHINDEX_API FMonolithDdcArtifactCache final : public IMonolithArtifactCache
{
public:
	FMonolithDdcArtifactCache();
	virtual ~FMonolithDdcArtifactCache() override;

	virtual TOptional<FMonolithArtifact> Get(
		const FMonolithArtifactIdentityV1& Identity,
		EMonolithArtifactCacheRequestMode RequestMode) override;
	virtual bool Put(
		const FMonolithArtifactIdentityV1& Identity,
		const FMonolithArtifact& Artifact,
		EMonolithArtifactCacheRequestMode RequestMode) override;
	virtual bool DrainRemoteWrites(double TimeoutSeconds = -1.0) override;
	virtual void DiscardPendingRemoteWrites() override;
	virtual FMonolithArtifactCacheStats GetStats() const override;
	virtual void ResetStats() override;
	virtual void SetIoThreadPool(FQueuedThreadPool* InIoThreadPool) override;

private:
	/** 把真正依赖 DDC 头文件和锁的细节藏到 cpp 里，减少编译耦合。 */
	struct FImpl;

	/** 把远端写队列调度到后台工作线程。 */
	static void ScheduleRemoteWriteWorker(const TSharedPtr<FImpl, ESPMode::ThreadSafe>& InImpl);
	/** 判断当前是否还有没收尾的远端写任务。 */
	static bool HasOutstandingRemoteWrites(const FImpl& InImpl);
	/** 把 breaker 的实时状态刷新到统计快照里。 */
	static void RefreshBreakerSnapshot(FImpl& InImpl, double NowSeconds);
	TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
};
