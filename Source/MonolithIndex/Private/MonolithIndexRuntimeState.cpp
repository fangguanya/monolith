#include "MonolithIndexRuntimeState.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonTypes.h"
#include "HAL/PlatformTime.h"

/*
 * runtime state 的重点不是“复杂”，而是“线程安全 + 解释性强”。
 *
 * 它把后台索引的各种进度数字整理成 UI 和 MCP 查询能直接消费的快照。
 */

namespace MonolithIndexRuntimeStateInternal
{
	/** 用稳定排序后的包数组构造统一的 cursor 分页结果。 */
	static TSharedPtr<FJsonObject> BuildPageFromSortedPackages(
		const TArray<FString>& SortedPackages,
		const int32 Limit,
		const FString& Cursor)
	{
		const int32 SafeLimit = FMath::Clamp(Limit, 1, 500);
		const int32 Offset = FMath::Clamp(FMonolithIndexRuntimeState::DecodeCursor(Cursor), 0, SortedPackages.Num());
		const int32 End = FMath::Min(Offset + SafeLimit, SortedPackages.Num());

		TArray<TSharedPtr<FJsonValue>> Results;
		Results.Reserve(End - Offset);
		for (int32 Index = Offset; Index < End; ++Index)
		{
			Results.Add(MakeShared<FJsonValueString>(SortedPackages[Index]));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("packages"), Results);
		Result->SetNumberField(TEXT("count"), Results.Num());
		Result->SetNumberField(TEXT("total"), SortedPackages.Num());
		Result->SetStringField(TEXT("next_cursor"), End < SortedPackages.Num() ? FMonolithIndexRuntimeState::EncodeCursor(End) : FString());
		return Result;
	}
}

void FMonolithIndexRuntimeState::Reset()
{
	FScopeLock Lock(&Mutex);
	StalePackages.Reset();
	CurrentProgressItems = 0;
	TotalProgressItems = 0;
	AnonymousTotalItems = 0;
	AnonymousOutstandingItems = 0;
	SessionStartSeconds = 0.0;
	bActive = false;
}

void FMonolithIndexRuntimeState::BeginSession(const TSet<FString>& InStalePackages, int32 InTotalProgressItems)
{
	FScopeLock Lock(&Mutex);
	// 开新会话时，旧进度和匿名工作一并清空。
	StalePackages = InStalePackages;
	CurrentProgressItems = 0;
	TotalProgressItems = FMath::Max(0, InTotalProgressItems);
	AnonymousTotalItems = 0;
	AnonymousOutstandingItems = 0;
	SessionStartSeconds = FPlatformTime::Seconds();
	bActive = true;
}

void FMonolithIndexRuntimeState::UpdateProgress(int32 InCurrentProgressItems, int32 InTotalProgressItems)
{
	FScopeLock Lock(&Mutex);
	// 总量至少不能小于当前完成量。
	CurrentProgressItems = FMath::Max(0, InCurrentProgressItems);
	TotalProgressItems = FMath::Max(CurrentProgressItems, InTotalProgressItems);
	if (TotalProgressItems > 0)
	{
		bActive = true;
		if (SessionStartSeconds <= 0.0)
		{
			SessionStartSeconds = FPlatformTime::Seconds();
		}
	}
}

void FMonolithIndexRuntimeState::BeginAnonymousWork(int32 Count)
{
	if (Count <= 0)
	{
		return;
	}

	FScopeLock Lock(&Mutex);
	// 匿名工作适合表示“已经开始忙，但还没法对应到具体包”的阶段。
	AnonymousTotalItems += Count;
	AnonymousOutstandingItems += Count;
	bActive = true;
	if (SessionStartSeconds <= 0.0)
	{
		SessionStartSeconds = FPlatformTime::Seconds();
	}
}

void FMonolithIndexRuntimeState::CompleteAnonymousWork(int32 Count)
{
	if (Count <= 0)
	{
		return;
	}

	FScopeLock Lock(&Mutex);
	// Outstanding 不能减成负数。
	AnonymousOutstandingItems = FMath::Max(0, AnonymousOutstandingItems - Count);
}

void FMonolithIndexRuntimeState::FinishSession()
{
	Reset();
}

bool FMonolithIndexRuntimeState::IsPackageStale(const FString& PackagePath) const
{
	FScopeLock Lock(&Mutex);
	return StalePackages.Contains(PackagePath);
}

void FMonolithIndexRuntimeState::AppendStalePackages(TSet<FString>& OutPackages) const
{
	FScopeLock Lock(&Mutex);
	OutPackages.Append(StalePackages);
}

FMonolithIndexRuntimeSnapshot FMonolithIndexRuntimeState::Snapshot() const
{
	FScopeLock Lock(&Mutex);

	FMonolithIndexRuntimeSnapshot Result;
	Result.bIndexingInProgress = bActive;
	Result.StalePackageCount = StalePackages.Num();

	const int32 RemainingProgressItems = FMath::Max(0, TotalProgressItems - CurrentProgressItems);
	Result.RemainingItems = RemainingProgressItems + AnonymousOutstandingItems;
	Result.QueueDepth = Result.RemainingItems;

	const int32 TotalWorkItems = TotalProgressItems + AnonymousTotalItems;
	const int32 CompletedAnonymousItems = AnonymousTotalItems - AnonymousOutstandingItems;
	const int32 CompletedItems = CurrentProgressItems + CompletedAnonymousItems;
	Result.CompletedItems = CompletedItems;
	Result.TotalItems = TotalWorkItems;

	if (TotalWorkItems > 0)
	{
		Result.Progress = FMath::Clamp(static_cast<double>(CompletedItems) / static_cast<double>(TotalWorkItems), 0.0, 1.0);
	}

	if (SessionStartSeconds > 0.0 && CompletedItems > 0 && Result.RemainingItems > 0)
	{
		// 用“平均每项耗时”来估 ETA，简单但足够直观。
		const double ElapsedSeconds = FPlatformTime::Seconds() - SessionStartSeconds;
		const double SecondsPerItem = ElapsedSeconds / static_cast<double>(CompletedItems);
		Result.EtaSeconds = SecondsPerItem * static_cast<double>(Result.RemainingItems);
	}

	return Result;
}

TSharedPtr<FJsonObject> FMonolithIndexRuntimeState::BuildStalePackagesPage(int32 Limit, const FString& Cursor) const
{
	TSet<FString> Packages;
	{
		FScopeLock Lock(&Mutex);
		Packages = StalePackages;
	}

	return BuildPackagePage(Packages, Limit, Cursor);
}

TSharedPtr<FJsonObject> FMonolithIndexRuntimeState::BuildPackagePage(
	const TSet<FString>& Packages,
	const int32 Limit,
	const FString& Cursor)
{
	TArray<FString> SortedPackages = Packages.Array();
	SortedPackages.Sort();

	return MonolithIndexRuntimeStateInternal::BuildPageFromSortedPackages(SortedPackages, Limit, Cursor);
}

TArray<FString> FMonolithIndexRuntimeState::NormalizePackagePrefixes(const TArray<FString>& InPrefixes)
{
	TArray<FString> Result;

	for (const FString& Prefix : InPrefixes)
	{
		// 做一些轻量清洗，保证比较时格式一致。
		FString Normalized = Prefix.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			continue;
		}

		while (Normalized.Len() > 1 && Normalized.EndsWith(TEXT("/")))
		{
			Normalized.LeftChopInline(1);
		}

		if (!Normalized.StartsWith(TEXT("/")))
		{
			Normalized = TEXT("/") + Normalized;
		}

		Result.AddUnique(Normalized);
	}

	return Result;
}

bool FMonolithIndexRuntimeState::PackageMatchesAnyPrefix(const FString& PackagePath, const TArray<FString>& Prefixes)
{
	// 既匹配完全相等，也匹配“位于这个前缀目录下面”。
	for (const FString& Prefix : Prefixes)
	{
		if (PackagePath == Prefix || PackagePath.StartsWith(Prefix + TEXT("/")))
		{
			return true;
		}
	}

	return false;
}

FString FMonolithIndexRuntimeState::EncodeCursor(int32 Offset)
{
	// 游标做成带版本号的字符串，未来改协议时更容易兼容。
	return FString::Printf(TEXT("v1:%d"), FMath::Max(0, Offset));
}

int32 FMonolithIndexRuntimeState::DecodeCursor(const FString& Cursor)
{
	if (Cursor.IsEmpty())
	{
		return 0;
	}

	FString Prefix;
	FString OffsetString;
	if (Cursor.Split(TEXT(":"), &Prefix, &OffsetString) && Prefix == TEXT("v1"))
	{
		return FMath::Max(0, FCString::Atoi(*OffsetString));
	}

	return 0;
}
