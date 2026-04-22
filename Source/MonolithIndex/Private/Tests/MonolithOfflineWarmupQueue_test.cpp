#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithOfflineWarmupQueue.h"

/*
 * 这组测试覆盖离线 warmup 队列文件的两类基本能力：
 * - 存档/读档 round-trip；
 * - 同目标去重，以及完成后移除。
 */

namespace MonolithOfflineWarmupQueueTestInternal
{
	/** 给每个测试创建独立的临时队列文件。 */
	static FString MakeTempQueuePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithOfflineWarmupQueue"), TEXT(".json"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithOfflineWarmupQueueRoundTripTest,
	"Monolith.Index.OfflineWarmupQueue.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOfflineWarmupQueueRoundTripTest::RunTest(const FString& Parameters)
{
	// 用临时文件避免污染真实 offline_warmup_queue.json。
	const FString QueuePath = MonolithOfflineWarmupQueueTestInternal::MakeTempQueuePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*QueuePath, false, true);
	};

	TArray<FMonolithOfflineWarmupRequest> Requests;
	FMonolithOfflineWarmupRequest Request;
	Request.PackagePath = TEXT("/Game/Test/SM_Offline");
	Request.AssetClass = TEXT("StaticMesh");
	Request.IndexerId = TEXT("GenericAsset");
	Request.Reason = TEXT("gt_breaker_or_quarantine");
	Request.EnqueuedAtUtc = TEXT("2026-04-20T08:00:00Z");
	Requests.Add(Request);

	TestTrue(TEXT("queue save should succeed"), SaveMonolithOfflineWarmupQueue(QueuePath, Requests));

	TArray<FMonolithOfflineWarmupRequest> LoadedRequests;
	TestTrue(TEXT("queue load should succeed"), LoadMonolithOfflineWarmupQueue(QueuePath, LoadedRequests));
	TestEqual(TEXT("one request should round-trip"), LoadedRequests.Num(), 1);
	TestEqual(TEXT("package path should round-trip"), LoadedRequests[0].PackagePath, Request.PackagePath);
	TestEqual(TEXT("indexer id should round-trip"), LoadedRequests[0].IndexerId, Request.IndexerId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithOfflineWarmupQueueDedupTest,
	"Monolith.Index.OfflineWarmupQueue.DeduplicatesAndRemoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOfflineWarmupQueueDedupTest::RunTest(const FString& Parameters)
{
	// 这里用相同 package/indexer 连续入队两次，验证后者会覆盖前者。
	const FString QueuePath = MonolithOfflineWarmupQueueTestInternal::MakeTempQueuePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*QueuePath, false, true);
	};

	FMonolithOfflineWarmupRequest First;
	First.PackagePath = TEXT("/Game/Test/T_Offline");
	First.AssetClass = TEXT("Texture2D");
	First.IndexerId = TEXT("GenericAsset");
	First.Reason = TEXT("gt_breaker");
	First.EnqueuedAtUtc = TEXT("2026-04-20T08:00:00Z");

	FMonolithOfflineWarmupRequest Second = First;
	Second.Reason = TEXT("gt_quarantine");
	Second.EnqueuedAtUtc = TEXT("2026-04-20T08:05:00Z");

	TestTrue(TEXT("first enqueue should succeed"), EnqueueMonolithOfflineWarmupRequest(QueuePath, First));
	TestTrue(TEXT("second enqueue should overwrite duplicate target"), EnqueueMonolithOfflineWarmupRequest(QueuePath, Second));

	TArray<FMonolithOfflineWarmupRequest> Requests;
	TestTrue(TEXT("queue should load"), LoadMonolithOfflineWarmupQueue(QueuePath, Requests));
	TestEqual(TEXT("duplicate package/indexer should be collapsed"), Requests.Num(), 1);
	TestEqual(TEXT("latest reason should win"), Requests[0].Reason, Second.Reason);

	const int32 RemovedCount = RemoveMonolithOfflineWarmupRequests(QueuePath, Requests);
	TestEqual(TEXT("remove should delete the queued request"), RemovedCount, 1);

	Requests.Reset();
	TestTrue(TEXT("queue should still load after removal"), LoadMonolithOfflineWarmupQueue(QueuePath, Requests));
	TestEqual(TEXT("queue should be empty after removal"), Requests.Num(), 0);
	return true;
}
