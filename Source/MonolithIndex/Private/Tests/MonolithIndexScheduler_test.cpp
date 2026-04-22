#include "Misc/AutomationTest.h"
#include "MonolithIndexScheduler.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"

/*
 * 这组测试守住 scheduler 骨架的三条底线：
 * - 配置会被合理钳制；
 * - cooperative stop 能让 drain 正常收尾；
 * - hard-cap 退出不会把 scheduler 永久打坏。
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexSchedulerClampTest,
	"Monolith.Index.Scheduler.ClampConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexSchedulerClampTest::RunTest(const FString& Parameters)
{
	// 故意塞进极端值，确认 scheduler 会自己纠正到安全区间。
	FMonolithIndexSchedulerConfig Config;
	Config.BackgroundCpuThreads = 0;
	Config.IoDdcThreads = 99;
	FMonolithIndexScheduler Scheduler(Config);
	TestEqual(TEXT("background pool should clamp to one thread"), Scheduler.GetBackgroundCpuThreadCount(), 1);
	TestEqual(TEXT("io/ddc pool should clamp to four threads"), Scheduler.GetIoDdcThreadCount(), 4);
	TestTrue(TEXT("default drain timeout should remain positive"), Scheduler.GetConfig().DrainTimeoutSeconds > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexSchedulerDrainTest,
	"Monolith.Index.Scheduler.DrainHonorsCooperativeStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexSchedulerDrainTest::RunTest(const FString& Parameters)
{
	// 用一个会自旋等待 stop 请求的后台 job，验证 drain 真能等到它协作退出。
	FMonolithIndexSchedulerConfig Config;
	Config.DrainTimeoutSeconds = 0.1;

	FMonolithIndexScheduler Scheduler(Config);
	TAtomic<bool> bObservedStop{false};
	FEvent* StartedEvent = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* FinishedEvent = FPlatformProcess::GetSynchEventFromPool(true);

	const bool bStarted = Scheduler.StartBackgroundJob([&Scheduler, &bObservedStop, StartedEvent, FinishedEvent]()
	{
		StartedEvent->Trigger();
		while (!Scheduler.IsStopRequested())
		{
			FPlatformProcess::Sleep(0.001f);
		}

		bObservedStop = true;
		FinishedEvent->Trigger();
	});

	TestTrue(TEXT("scheduler should start the background job"), bStarted);
	TestTrue(TEXT("background job should begin running"), StartedEvent->Wait(1000));

	Scheduler.RequestStop();

	TestTrue(TEXT("background job should observe the stop request"), FinishedEvent->Wait(2000));
	TestTrue(TEXT("scheduler should drain after cooperative stop"), Scheduler.WaitForDrain(1.0));
	TestTrue(TEXT("job should record the stop signal"), bObservedStop.Load());
	TestTrue(
		TEXT("cooperative shutdown should finish cleanly once the job has drained"),
		Scheduler.Shutdown(EMonolithSchedulerShutdownMode::WaitForCooperativeDrain));

	FPlatformProcess::ReturnSynchEventToPool(StartedEvent);
	FPlatformProcess::ReturnSynchEventToPool(FinishedEvent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexSchedulerHardCapTest,
	"Monolith.Index.Scheduler.HardCapTimeoutCanBeRecovered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexSchedulerHardCapTest::RunTest(const FString& Parameters)
{
	/*
	 * 这条测试模拟“后台 job 一时半会儿不肯停”的场景：
	 * - 第一次 Shutdown 走 hard-cap 模式，应该快速返回 false；
	 * - 等 job 自己后来结束后，再次 Shutdown 应该还能把线程池干净关掉。
	 *
	 * 这样我们就能同时守住两件事：
	 * 1. 退出路径不再无限等；
	 * 2. 这套语义不是一次性的“只能泄漏后跑路”。
	 */
	FMonolithIndexSchedulerConfig Config;
	Config.DrainTimeoutSeconds = 0.05;

	FMonolithIndexScheduler Scheduler(Config);
	FEvent* StartedEvent = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* ReleaseEvent = FPlatformProcess::GetSynchEventFromPool(true);

	const bool bStarted = Scheduler.StartBackgroundJob([StartedEvent, ReleaseEvent]()
	{
		StartedEvent->Trigger();
		ReleaseEvent->Wait();
	});
	TestTrue(TEXT("scheduler should start the blocking background job"), bStarted);
	TestTrue(TEXT("blocking job should begin running"), StartedEvent->Wait(1000));

	const double ShutdownStartSeconds = FPlatformTime::Seconds();
	const bool bDrainedWithinHardCap = Scheduler.Shutdown(EMonolithSchedulerShutdownMode::AllowProcessExitAbandon);
	const double ShutdownElapsedSeconds = FPlatformTime::Seconds() - ShutdownStartSeconds;
	TestFalse(TEXT("hard-cap shutdown should report timeout while the job is still blocked"), bDrainedWithinHardCap);
	TestTrue(TEXT("hard-cap shutdown should return quickly instead of waiting forever"), ShutdownElapsedSeconds < 0.50);

	ReleaseEvent->Trigger();
	TestTrue(TEXT("scheduler should eventually drain after the blocking job is released"), Scheduler.WaitForDrain(1.0));
	TestTrue(
		TEXT("a later cooperative shutdown should still be able to cleanly destroy the pools"),
		Scheduler.Shutdown(EMonolithSchedulerShutdownMode::WaitForCooperativeDrain));

	FPlatformProcess::ReturnSynchEventToPool(StartedEvent);
	FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	return true;
}
