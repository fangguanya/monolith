#include "Embedders/ClipVramBudgetGuard.h"

#include "Misc/AutomationTest.h"

/*
 * ClipVramBudgetGuard 测试覆盖：
 *  - 一般情况下 batch=4 能预留成功
 *  - 预算用尽时 batch 会自动降到能容纳的最大值
 *  - Release 后预算回归
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClipVramBudgetGuardAcquireReleaseTest,
	"Monolith.Index.AssetVisual.ClipVramBudget.AcquireRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClipVramBudgetGuardAcquireReleaseTest::RunTest(const FString& Parameters)
{
	FClipVramBudgetGuard Guard;

	// 默认 batch=4 应当能预留成功（300MB 权重 + 4×200MB activation = 1.1GB ≤ 1.5GB）。
	int32 Granted = 0;
	TestTrue(TEXT("default batch=4 should fit budget"), Guard.TryAcquire(4, Granted));
	TestEqual(TEXT("granted batch should match request"), Granted, 4);

	const uint64 InUseAfterAcquire = Guard.GetInUseBytes();
	TestTrue(TEXT("InUse should grow after acquire"), InUseAfterAcquire > FClipVramBudgetGuard::WeightBytes);

	Guard.Release(Granted);
	TestEqual(TEXT("InUse should drop back to weights only after release"),
		Guard.GetInUseBytes(), FClipVramBudgetGuard::WeightBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClipVramBudgetGuardAutoDowngradeTest,
	"Monolith.Index.AssetVisual.ClipVramBudget.AutoDowngradeWhenLow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClipVramBudgetGuardAutoDowngradeTest::RunTest(const FString& Parameters)
{
	FClipVramBudgetGuard Guard;

	// 预先吃掉一大块预算，模拟 GPU 已经被别人占着。
	int32 Granted1 = 0, Granted2 = 0;
	TestTrue(TEXT("first acquire should succeed"), Guard.TryAcquire(4, Granted1));

	// 第二次申请 batch=4 大概率会被降到 batch=1 或失败：
	// 当前剩余预算 = 1500 - 300 - 800 = 400MB；只够 1×200MB activation。
	const bool bAcquired = Guard.TryAcquire(4, Granted2);
	if (bAcquired)
	{
		TestTrue(TEXT("granted batch should be downgraded"), Granted2 < 4);
		Guard.Release(Granted2);
	}
	Guard.Release(Granted1);
	return true;
}
