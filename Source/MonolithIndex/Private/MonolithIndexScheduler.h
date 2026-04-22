#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Misc/QueuedThreadPool.h"

/*
 * 这份头文件定义了 MonolithIndex 的调度骨架。
 *
 * 以前的做法更像“开一根后台线程，然后很多地方自己等事件”。
 * 现在改成调度器之后，我们把资源明确分成两类：
 * - BackgroundCpuPool：普通后台 CPU 工作；
 * - IoDdcPool：偏 IO / DDC 的工作；
 *
 * 这能解决一个很现实的问题：
 * “任务看起来在后台，实际卡顿却发生在前台 GT。”
 * 所以这里的重点不是线程数量，而是把 CPU / IO 两类后台资源收口到唯一入口，
 * 让索引链不再偷偷夹带一套额外的 GT 等待旁路。
 */

struct FMonolithIndexSchedulerConfig
{
	/** 普通后台 CPU 线程数。 */
	int32 BackgroundCpuThreads = 1;
	/** 偏 IO / DDC 的线程数。 */
	int32 IoDdcThreads = 2;
	/** 停机等待 drain 的默认超时时间。 */
	double DrainTimeoutSeconds = 5.0;
};

/** 根据设置文件生成一份合理的默认调度配置。 */
MONOLITHINDEX_API FMonolithIndexSchedulerConfig BuildDefaultMonolithIndexSchedulerConfig();

/*
 * 调度器停机时有两种明确模式：
 *
 * - WaitForCooperativeDrain：
 *   普通对象生命周期下使用。先等默认超时，如果还没排空，就继续协作式等待直到结束。
 *
 * - AllowProcessExitAbandon：
 *   编辑器/commandlet 退出路径使用。先等默认超时；
 *   如果还没排空，就不再继续无限等，而是在析构时放弃线程池所有权，让进程退出继续往前走。
 *
 * 这样“正常收尾”和“进程退出硬上限”就不会再混在一套语义里。
 */
enum class EMonolithSchedulerShutdownMode : uint8
{
	/** 默认语义：一定等到协作式排空。 */
	WaitForCooperativeDrain,
	/** 进程退出语义：超时后允许放弃线程池所有权，不再无限等待。 */
	AllowProcessExitAbandon,
};

class FMonolithNamedThreadPool
{
public:
	FMonolithNamedThreadPool() = default;
	/** 析构时自动关闭线程池，避免后台线程泄漏。 */
	~FMonolithNamedThreadPool();

	/** 创建一个带名字的线程池。 */
	bool Initialize(const TCHAR* InName, int32 InThreadCount, EThreadPriority InThreadPriority);
	/** 销毁线程池并等待内部资源释放。 */
	void Shutdown();
	/** 退出流程里放弃线程池所有权，不再阻塞等它自己收尾。
	 * 这只应该用于“进程马上就要退出”的路径。 */
	void AbandonForProcessExit();

	/** 当前是否已成功初始化。 */
	bool IsInitialized() const { return Pool.IsValid(); }
	/** 当前线程池里有多少工作线程。 */
	int32 GetThreadCount() const { return ThreadCount; }
	/** 拿到底层 UE 线程池指针。 */
	FQueuedThreadPool* Get() const { return Pool.Get(); }

private:
	/** 线程池名字，方便调试。 */
	FString Name;
	/** 实际创建出来的线程数。 */
	int32 ThreadCount = 0;
	/** UE 的底层线程池对象。 */
	TUniquePtr<FQueuedThreadPool> Pool;
};

class FMonolithIndexScheduler
{
public:
	/** 构造调度器并创建内部线程池。 */
	explicit FMonolithIndexScheduler(const FMonolithIndexSchedulerConfig& InConfig = BuildDefaultMonolithIndexSchedulerConfig());
	/** 析构时自动请求停止并关停资源。 */
	~FMonolithIndexScheduler();

	/** 把一个后台 job 投递到 CPU 线程池。 */
	bool StartBackgroundJob(TUniqueFunction<void()> Work, EQueuedWorkPriority Priority = EQueuedWorkPriority::Low);
	/** 发出协作式停止请求。 */
	void RequestStop();
	/** 外部是否已经请求停止。 */
	bool IsStopRequested() const { return SharedState->bStopRequested.Load(); }
	/** 当前是否还有活跃 job 在运行。 */
	bool IsRunning() const { return SharedState->bRunning.Load(); }
	/** 等待当前 job 排空。 */
	bool WaitForDrain(double TimeoutSeconds = -1.0);
	/** 统一停机入口。
	 * 返回 true 表示已经在本轮调用里排空；
	 * 返回 false 表示命中了硬上限，后续将改由析构路径放弃线程池所有权。 */
	bool Shutdown(EMonolithSchedulerShutdownMode Mode = EMonolithSchedulerShutdownMode::WaitForCooperativeDrain);

	/** 背景 CPU 池线程数。 */
	int32 GetBackgroundCpuThreadCount() const { return BackgroundCpuPool.GetThreadCount(); }
	/** IO/DDC 池线程数。 */
	int32 GetIoDdcThreadCount() const { return IoDdcPool.GetThreadCount(); }
	/** 给其它组件借用的 IO/DDC 线程池。 */
	FQueuedThreadPool* GetIoDdcThreadPool() const { return IoDdcPool.Get(); }
	/** 当前使用的配置。 */
	const FMonolithIndexSchedulerConfig& GetConfig() const { return Config; }

private:
	/*
	 * 这份共享状态故意和调度器对象本体拆开。
	 *
	 * 原因很直接：
	 * - 后台 job 会在 lambda 结束时回写“我已经跑完了”；
	 * - 但进程退出路径里，我们可能不想再无限等调度器对象自己活着。
	 *
	 * 把状态独立成共享对象后，后台 job 就只需要抓住 SharedState，
	 * 不会在超时退出路径里再回调一个已经析构的 `this`。
	 */
	struct FSharedState
	{
		/** 保护 future 句柄与排空状态。 */
		mutable FCriticalSection StateMutex;
		/** 当前活跃后台 job 的 future。 */
		TUniquePtr<TFuture<void>> ActiveFuture;
		/** 是否存在运行中的 job。 */
		TAtomic<bool> bRunning{false};
		/** 是否收到了停止请求。 */
		TAtomic<bool> bStopRequested{false};
	};

	/** 后台 job 自己结束时回调这里，把运行状态收尾干净。 */
	static void MarkJobFinished(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& InState);

	/** 调度配置。 */
	FMonolithIndexSchedulerConfig Config;
	/** 普通后台 CPU 池。 */
	FMonolithNamedThreadPool BackgroundCpuPool;
	/** 偏 IO / DDC 的线程池。 */
	FMonolithNamedThreadPool IoDdcPool;
	/** 后台 job 的共享状态。 */
	TSharedRef<FSharedState, ESPMode::ThreadSafe> SharedState;
	/** 是否已经完整关停过线程池。 */
	bool bShutdownCompleted = false;
	/** 是否已经在退出路径上承诺“析构时允许放弃线程池所有权”。 */
	bool bAbandonOnDestruction = false;
};
