#include "MonolithIndexScheduler.h"

#include "MonolithIndexLog.h"
#include "MonolithSettings.h"

#include "Misc/ScopeExit.h"

/*
 * 这份实现文件把“调度”这件事拆成了三个很具体的动作：
 * 1. 创建和销毁线程池；
 * 2. 安全地把工作切回 GameThread 再等它做完；
 * 3. 在停机时尽量先等任务自己收尾，而不是粗暴把线程砍掉。
 *
 * 这里没有追求“最炫的并发模型”，而是追求一件更重要的事：
 * “出现意外时，系统也能用一种可解释的方式停下来。”
 */

/** 从设置对象生成一份调度默认值。 */
FMonolithIndexSchedulerConfig BuildDefaultMonolithIndexSchedulerConfig()
{
	FMonolithIndexSchedulerConfig Config;
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	Config.IoDdcThreads = FMath::Clamp(Settings ? Settings->IoDdcPoolSize : 2, 1, 4);
	Config.BackgroundCpuThreads = 1;
	Config.DrainTimeoutSeconds = 5.0;
	return Config;
}

/** 析构时统一走 Shutdown，避免线程池遗留。 */
FMonolithNamedThreadPool::~FMonolithNamedThreadPool()
{
	Shutdown();
}

bool FMonolithNamedThreadPool::Initialize(const TCHAR* InName, const int32 InThreadCount, const EThreadPriority InThreadPriority)
{
	// 先把旧线程池关掉，再创建新线程池，避免半初始化状态残留。
	Shutdown();

	Name = InName ? InName : TEXT("MonolithThreadPool");
	ThreadCount = FMath::Max(1, InThreadCount);
	Pool.Reset(FQueuedThreadPool::Allocate());
	if (!Pool.IsValid())
	{
		// 连线程池对象都分配不出来，说明这次初始化完全失败了。
		ThreadCount = 0;
		return false;
	}

	if (!Pool->Create(ThreadCount, 0, InThreadPriority, *Name))
	{
		// Create 失败时要把状态恢复到“未初始化”，免得外界误判还能使用。
		Pool.Reset();
		ThreadCount = 0;
		return false;
	}

	return true;
}

void FMonolithNamedThreadPool::Shutdown()
{
	if (Pool.IsValid())
	{
		// Destroy 会等待 UE 线程池自身做资源清理。
		Pool->Destroy();
		Pool.Reset();
	}

	ThreadCount = 0;
	Name.Reset();
}

void FMonolithNamedThreadPool::AbandonForProcessExit()
{
	if (Pool.IsValid())
	{
		/*
		 * 这里故意不再调用 Destroy。
		 *
		 * 原因是：
		 * - 进程退出路径已经命中了 hard cap；
		 * - 如果这时还继续 Destroy，就会重新回到“无限等线程池自己收尾”的老问题。
		 *
		 * 所以我们只在“进程马上就要退出”的模式下放弃线程池所有权，
		 * 剩余资源交给进程退出时的操作系统清理。
		 */
		FQueuedThreadPool* LeakedPool = Pool.Release();
		(void)LeakedPool;
	}

	ThreadCount = 0;
	Name.Reset();
}

FMonolithIndexScheduler::FMonolithIndexScheduler(const FMonolithIndexSchedulerConfig& InConfig)
	: Config(InConfig)
	, SharedState(MakeShared<FSharedState, ESPMode::ThreadSafe>())
{
	// 调度器启动时就把两类后台资源池都准备好，后面任务提交才能走统一入口。
	const bool bBackgroundOk = BackgroundCpuPool.Initialize(
		TEXT("MonolithBackgroundCpuPool"),
		FMath::Max(1, Config.BackgroundCpuThreads),
		TPri_BelowNormal);
	const bool bIoOk = IoDdcPool.Initialize(
		TEXT("MonolithIoDdcPool"),
		FMath::Clamp(Config.IoDdcThreads, 1, 4),
		TPri_BelowNormal);

	if (!bBackgroundOk || !bIoOk)
	{
		UE_LOG(
			LogMonolithIndex,
			Warning,
			TEXT("Monolith scheduler pool initialization incomplete (background=%s, io=%s)"),
			bBackgroundOk ? TEXT("ok") : TEXT("failed"),
			bIoOk ? TEXT("ok") : TEXT("failed"));
	}
}

FMonolithIndexScheduler::~FMonolithIndexScheduler()
{
	if (bShutdownCompleted)
	{
		return;
	}

	if (bAbandonOnDestruction)
	{
		// 只有“已经明确允许进程退出放弃所有权”的路径，
		// 才能在析构时直接放弃线程池，不再继续等待。
		BackgroundCpuPool.AbandonForProcessExit();
		IoDdcPool.AbandonForProcessExit();
		bShutdownCompleted = true;
		return;
	}

	Shutdown(IsEngineExitRequested()
		? EMonolithSchedulerShutdownMode::AllowProcessExitAbandon
		: EMonolithSchedulerShutdownMode::WaitForCooperativeDrain);
}

/** 向后台 CPU 池提交一个任务。 */
bool FMonolithIndexScheduler::StartBackgroundJob(TUniqueFunction<void()> Work, const EQueuedWorkPriority Priority)
{
	if (!Work || !BackgroundCpuPool.IsInitialized())
	{
		return false;
	}

	const TSharedRef<FSharedState, ESPMode::ThreadSafe> LocalState = SharedState;
	FScopeLock Lock(&LocalState->StateMutex);
	if (LocalState->bRunning.Load())
	{
		// 当前只允许一个主 job 在跑，避免多个大任务相互打架。
		return false;
	}

	bShutdownCompleted = false;
	bAbandonOnDestruction = false;
	LocalState->bStopRequested = false;
	LocalState->bRunning = true;

	LocalState->ActiveFuture = MakeUnique<TFuture<void>>(AsyncPool(
		*BackgroundCpuPool.Get(),
		[LocalState, Work = MoveTemp(Work)]() mutable
		{
			ON_SCOPE_EXIT
			{
				// 无论 job 正常结束还是中途提前 return，都要把运行状态收尾干净。
				MarkJobFinished(LocalState);
			};

			if (Work)
			{
				Work();
			}
		},
		nullptr,
		Priority));

	return true;
}

void FMonolithIndexScheduler::RequestStop()
{
	// 这里只是打“请停下”的旗子，真正怎么停由 job 自己协作处理。
	SharedState->bStopRequested = true;
}

/** 等待当前 job 排空。 */
bool FMonolithIndexScheduler::WaitForDrain(double TimeoutSeconds)
{
	const TSharedRef<FSharedState, ESPMode::ThreadSafe> LocalState = SharedState;
	// -1 表示“用配置里的默认超时”，<-1 表示“无限等到它自己结束”。
	const bool bUseConfiguredTimeout = FMath::IsNearlyEqual(TimeoutSeconds, -1.0);
	const bool bInfiniteWait = TimeoutSeconds < -1.0;
	const double EffectiveTimeout = bInfiniteWait
		? 0.0
		: (bUseConfiguredTimeout ? Config.DrainTimeoutSeconds : TimeoutSeconds);
	const double DeadlineSeconds = bInfiniteWait ? 0.0 : (FPlatformTime::Seconds() + EffectiveTimeout);

	while (true)
	{
		{
			FScopeLock Lock(&LocalState->StateMutex);
			if (!LocalState->ActiveFuture.IsValid())
			{
				// 已经没有活跃 future，说明排空完成。
				return true;
			}

			if (LocalState->ActiveFuture->IsReady())
			{
				// future 已经 ready，就再 Wait 一次把尾巴收干净，然后清空句柄。
				LocalState->ActiveFuture->Wait();
				LocalState->ActiveFuture.Reset();
				return true;
			}
		}

		if (!bInfiniteWait && FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			return false;
		}

		FPlatformProcess::Sleep(0.01f);
	}
}

/** 调度器停机入口。 */
bool FMonolithIndexScheduler::Shutdown(const EMonolithSchedulerShutdownMode Mode)
{
	if (bShutdownCompleted)
	{
		return true;
	}

	RequestStop();

	const bool bDrainedBeforeTimeout = WaitForDrain(Config.DrainTimeoutSeconds);
	if (!bDrainedBeforeTimeout)
	{
		if (Mode == EMonolithSchedulerShutdownMode::AllowProcessExitAbandon)
		{
			/*
			 * 这里不再继续无限等待。
			 *
			 * 取而代之的是记下“析构时允许放弃线程池所有权”，
			 * 这样退出流程就能真正遵守 hard cap；
			 * 同时对象本体在当前作用域里仍然活着，调用方如果后面又等到了 drain，
			 * 仍然可以再调用一次 Shutdown 把线程池正常关掉。
			 */
			bAbandonOnDestruction = true;
			UE_LOG(
				LogMonolithIndex,
				Warning,
				TEXT("Monolith scheduler drain exceeded %.2fs; scheduler will abandon thread pools on destruction so process exit can continue"),
				Config.DrainTimeoutSeconds);
			return false;
		}

		// 非退出路径仍然坚持协作式收尾，保证对象生命周期内不留下悬空线程池。
		UE_LOG(
			LogMonolithIndex,
			Warning,
			TEXT("Monolith scheduler drain exceeded %.2fs; waiting for cooperative shutdown"),
			Config.DrainTimeoutSeconds);
		WaitForDrain(-2.0);
	}

	BackgroundCpuPool.Shutdown();
	IoDdcPool.Shutdown();
	bShutdownCompleted = true;
	bAbandonOnDestruction = false;
	return true;
}

void FMonolithIndexScheduler::MarkJobFinished(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& InState)
{
	// job 结束时把运行标记放下，外层才能再提交下一轮。
	InState->bRunning = false;
}
