#include "MonolithOfflineWarmupQueue.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * 离线 warmup 队列本质上就是一个 JSON 文件。
 *
 * 这份文件帮助系统记住：
 * “哪些包虽然现在没空立刻 warmup，但下次离线窗口里要补回来。”
 */

namespace MonolithOfflineWarmupQueueInternal
{
	/** 统一排序，保证文件内容稳定，便于 diff 和排查。 */
	static void SortRequests(TArray<FMonolithOfflineWarmupRequest>& Requests)
	{
		Requests.Sort([](const FMonolithOfflineWarmupRequest& A, const FMonolithOfflineWarmupRequest& B)
		{
			if (A.PackagePath != B.PackagePath)
			{
				return A.PackagePath < B.PackagePath;
			}
			return A.IndexerId < B.IndexerId;
		});
	}

	/** 把一条请求转成 JSON 对象。 */
	static TSharedPtr<FJsonObject> ToJson(const FMonolithOfflineWarmupRequest& Request)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("package_path"), Request.PackagePath);
		Json->SetStringField(TEXT("asset_class"), Request.AssetClass);
		Json->SetStringField(TEXT("indexer_id"), Request.IndexerId);
		Json->SetStringField(TEXT("reason"), Request.Reason);
		Json->SetStringField(TEXT("enqueued_at_utc"), Request.EnqueuedAtUtc);
		return Json;
	}

	/** 从 JSON 对象还原一条请求。 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FMonolithOfflineWarmupRequest& OutRequest)
	{
		if (!Json.IsValid())
		{
			return false;
		}

		if (!Json->TryGetStringField(TEXT("package_path"), OutRequest.PackagePath)
			|| !Json->TryGetStringField(TEXT("indexer_id"), OutRequest.IndexerId))
		{
			return false;
		}

		Json->TryGetStringField(TEXT("asset_class"), OutRequest.AssetClass);
		Json->TryGetStringField(TEXT("reason"), OutRequest.Reason);
		Json->TryGetStringField(TEXT("enqueued_at_utc"), OutRequest.EnqueuedAtUtc);
		return !OutRequest.PackagePath.IsEmpty() && !OutRequest.IndexerId.IsEmpty();
	}
}

FString GetMonolithOfflineWarmupQueuePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithIndex"), TEXT("offline_warmup_queue.json"));
}

bool LoadMonolithOfflineWarmupQueue(const FString& FilePath, TArray<FMonolithOfflineWarmupRequest>& OutRequests)
{
	OutRequests.Reset();

	if (!IFileManager::Get().FileExists(*FilePath))
	{
		// 文件不存在不算错误，表示当前队列为空。
		return true;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& JsonValue : JsonArray)
	{
		FMonolithOfflineWarmupRequest Request;
		if (MonolithOfflineWarmupQueueInternal::FromJson(JsonValue.IsValid() ? JsonValue->AsObject() : nullptr, Request))
		{
			OutRequests.Add(MoveTemp(Request));
		}
	}

	MonolithOfflineWarmupQueueInternal::SortRequests(OutRequests);
	return true;
}

bool SaveMonolithOfflineWarmupQueue(const FString& FilePath, const TArray<FMonolithOfflineWarmupRequest>& Requests)
{
	// 先确保目录存在，再写 JSON 文件。
	FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	TArray<FMonolithOfflineWarmupRequest> SortedRequests = Requests;
	MonolithOfflineWarmupQueueInternal::SortRequests(SortedRequests);

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	JsonArray.Reserve(SortedRequests.Num());
	for (const FMonolithOfflineWarmupRequest& Request : SortedRequests)
	{
		JsonArray.Add(MakeShared<FJsonValueObject>(MonolithOfflineWarmupQueueInternal::ToJson(Request)));
	}

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(JsonArray, Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool EnqueueMonolithOfflineWarmupRequest(const FString& FilePath, const FMonolithOfflineWarmupRequest& Request)
{
	if (Request.PackagePath.IsEmpty() || Request.IndexerId.IsEmpty())
	{
		// 缺少主键字段就不能入队。
		return false;
	}

	TArray<FMonolithOfflineWarmupRequest> Requests;
	if (!LoadMonolithOfflineWarmupQueue(FilePath, Requests))
	{
		return false;
	}

	for (FMonolithOfflineWarmupRequest& Existing : Requests)
	{
		if (Existing.MatchesTarget(Request))
		{
			// 已存在同目标项时，不重复新增，而是覆盖最新说明。
			Existing.AssetClass = Request.AssetClass;
			Existing.Reason = Request.Reason;
			Existing.EnqueuedAtUtc = Request.EnqueuedAtUtc;
			return SaveMonolithOfflineWarmupQueue(FilePath, Requests);
		}
	}

	Requests.Add(Request);
	return SaveMonolithOfflineWarmupQueue(FilePath, Requests);
}

int32 RemoveMonolithOfflineWarmupRequests(const FString& FilePath, const TArray<FMonolithOfflineWarmupRequest>& CompletedRequests)
{
	if (CompletedRequests.Num() == 0)
	{
		return 0;
	}

	TArray<FMonolithOfflineWarmupRequest> Requests;
	if (!LoadMonolithOfflineWarmupQueue(FilePath, Requests))
	{
		return 0;
	}

	const int32 OriginalCount = Requests.Num();
	Requests.RemoveAll([&CompletedRequests](const FMonolithOfflineWarmupRequest& Existing)
	{
		for (const FMonolithOfflineWarmupRequest& Completed : CompletedRequests)
		{
			if (Existing.MatchesTarget(Completed))
			{
				return true;
			}
		}
		return false;
	});

	if (Requests.Num() == OriginalCount)
	{
		return 0;
	}

	if (Requests.Num() == 0)
	{
		// 队列清空后直接删除文件，让“空队列”状态更干净。
		IFileManager::Get().Delete(*FilePath, false, true);
	}
	else
	{
		SaveMonolithOfflineWarmupQueue(FilePath, Requests);
	}

	return OriginalCount - Requests.Num();
}

bool LoadMonolithOfflineWarmupQueue(TArray<FMonolithOfflineWarmupRequest>& OutRequests)
{
	return LoadMonolithOfflineWarmupQueue(GetMonolithOfflineWarmupQueuePath(), OutRequests);
}

bool SaveMonolithOfflineWarmupQueue(const TArray<FMonolithOfflineWarmupRequest>& Requests)
{
	return SaveMonolithOfflineWarmupQueue(GetMonolithOfflineWarmupQueuePath(), Requests);
}

bool EnqueueMonolithOfflineWarmupRequest(const FMonolithOfflineWarmupRequest& Request)
{
	return EnqueueMonolithOfflineWarmupRequest(GetMonolithOfflineWarmupQueuePath(), Request);
}

int32 RemoveMonolithOfflineWarmupRequests(const TArray<FMonolithOfflineWarmupRequest>& CompletedRequests)
{
	return RemoveMonolithOfflineWarmupRequests(GetMonolithOfflineWarmupQueuePath(), CompletedRequests);
}

bool IsPackageQueuedForMonolithOfflineWarmup(const FString& PackagePath)
{
	TArray<FMonolithOfflineWarmupRequest> Requests;
	if (!LoadMonolithOfflineWarmupQueue(Requests))
	{
		return false;
	}

	for (const FMonolithOfflineWarmupRequest& Request : Requests)
	{
		if (Request.PackagePath.Equals(PackagePath, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

void AppendMonolithOfflineWarmupQueuedPackages(TSet<FString>& OutPackages)
{
	TArray<FMonolithOfflineWarmupRequest> Requests;
	if (!LoadMonolithOfflineWarmupQueue(Requests))
	{
		return;
	}

	for (const FMonolithOfflineWarmupRequest& Request : Requests)
	{
		OutPackages.Add(Request.PackagePath);
	}
}
