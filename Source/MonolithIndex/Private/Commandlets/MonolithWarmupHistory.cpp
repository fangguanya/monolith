#include "Commandlets/MonolithWarmupHistory.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * warmup history 是 release gate 的“记忆”。
 *
 * 单次 warmup 跑完只告诉我们“这一次”命中率怎么样；
 * 但 gate 往往需要看“连续几次是否都稳定达标”。
 * 所以这里负责把历史写成 JSON，并提供连续达标统计。
 */

namespace MonolithWarmupHistoryInternal
{
	/** 把一条运行记录转成 JSON。 */
	static TSharedPtr<FJsonObject> ToJson(const FMonolithWarmupRunRecord& Run)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("scope_key"), Run.ScopeKey);
		Json->SetStringField(TEXT("scope_display"), Run.ScopeDisplay);
		Json->SetStringField(TEXT("started_at_utc"), Run.StartedAtUtc);
		Json->SetNumberField(TEXT("attempted_packages"), Run.AttemptedPackages);
		Json->SetNumberField(TEXT("warmed_packages"), Run.WarmedPackages);
		Json->SetNumberField(TEXT("cache_hit_rate_percent"), Run.CacheHitRatePercent);
		Json->SetNumberField(TEXT("local_hit"), static_cast<double>(Run.LocalHitCount));
		Json->SetNumberField(TEXT("remote_hit"), static_cast<double>(Run.RemoteHitCount));
		Json->SetNumberField(TEXT("remote_miss"), static_cast<double>(Run.RemoteMissCount));
		Json->SetNumberField(TEXT("remote_write_ok"), static_cast<double>(Run.RemoteWriteOkCount));
		Json->SetNumberField(TEXT("remote_write_fail"), static_cast<double>(Run.RemoteWriteFailCount));
		Json->SetBoolField(TEXT("threshold_met"), Run.bThresholdMet);
		return Json;
	}

	/** 从 JSON 对象还原一条运行记录。 */
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FMonolithWarmupRunRecord& OutRun)
	{
		if (!Json.IsValid())
		{
			return false;
		}

		if (!Json->TryGetStringField(TEXT("scope_key"), OutRun.ScopeKey))
		{
			return false;
		}

		Json->TryGetStringField(TEXT("scope_display"), OutRun.ScopeDisplay);
		Json->TryGetStringField(TEXT("started_at_utc"), OutRun.StartedAtUtc);

		double NumberValue = 0.0;
		if (Json->TryGetNumberField(TEXT("attempted_packages"), NumberValue))
		{
			OutRun.AttemptedPackages = static_cast<int32>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("warmed_packages"), NumberValue))
		{
			OutRun.WarmedPackages = static_cast<int32>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("cache_hit_rate_percent"), OutRun.CacheHitRatePercent))
		{
		}
		if (Json->TryGetNumberField(TEXT("local_hit"), NumberValue))
		{
			OutRun.LocalHitCount = static_cast<uint64>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("remote_hit"), NumberValue))
		{
			OutRun.RemoteHitCount = static_cast<uint64>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("remote_miss"), NumberValue))
		{
			OutRun.RemoteMissCount = static_cast<uint64>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("remote_write_ok"), NumberValue))
		{
			OutRun.RemoteWriteOkCount = static_cast<uint64>(NumberValue);
		}
		if (Json->TryGetNumberField(TEXT("remote_write_fail"), NumberValue))
		{
			OutRun.RemoteWriteFailCount = static_cast<uint64>(NumberValue);
		}
		Json->TryGetBoolField(TEXT("threshold_met"), OutRun.bThresholdMet);
		return !OutRun.ScopeKey.IsEmpty();
	}
}

FString GetMonolithWarmupHistoryPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithIndex"), TEXT("warmup_history.json"));
}

bool LoadMonolithWarmupHistory(const FString& FilePath, TArray<FMonolithWarmupRunRecord>& OutRuns)
{
	OutRuns.Reset();

	if (!IFileManager::Get().FileExists(*FilePath))
	{
		// 文件还不存在不算错误，只表示目前没有历史。
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
		FMonolithWarmupRunRecord Run;
		if (MonolithWarmupHistoryInternal::FromJson(JsonValue.IsValid() ? JsonValue->AsObject() : nullptr, Run))
		{
			OutRuns.Add(MoveTemp(Run));
		}
	}

	return true;
}

bool SaveMonolithWarmupHistory(const FString& FilePath, const TArray<FMonolithWarmupRunRecord>& Runs)
{
	// 先确保目录存在，再写 JSON 文件。
	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	JsonArray.Reserve(Runs.Num());
	for (const FMonolithWarmupRunRecord& Run : Runs)
	{
		JsonArray.Add(MakeShared<FJsonValueObject>(MonolithWarmupHistoryInternal::ToJson(Run)));
	}

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(JsonArray, Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool AppendMonolithWarmupHistoryRecord(const FString& FilePath, const FMonolithWarmupRunRecord& Run, const int32 MaxEntries)
{
	// 追加逻辑统一复用 load/save，保持文件格式单一。
	TArray<FMonolithWarmupRunRecord> Runs;
	if (!LoadMonolithWarmupHistory(FilePath, Runs))
	{
		return false;
	}

	Runs.Add(Run);
	if (MaxEntries > 0 && Runs.Num() > MaxEntries)
	{
		// 超过上限时，丢掉最老的一段记录。
		Runs.RemoveAt(0, Runs.Num() - MaxEntries, EAllowShrinking::No);
	}

	return SaveMonolithWarmupHistory(FilePath, Runs);
}

double ComputeMonolithWarmupHitRatePercent(const FMonolithArtifactCacheStats& Stats, const int32 AttemptedPackages)
{
	if (AttemptedPackages <= 0)
	{
		return 0.0;
	}

	// warmup 的 hit rate 只关心“命中了多少包”，不把远端写入算进去。
	const uint64 TotalHits = Stats.LocalHitCount + Stats.RemoteHitCount;
	return (static_cast<double>(TotalHits) * 100.0) / static_cast<double>(AttemptedPackages);
}

int32 NormalizeMonolithWarmupReleaseThresholdPercent(const int32 ThresholdPercent)
{
	// release gate 的单位固定就是“百分比整数”，
	// 所以运行时也要强制钳到 0-100，不能只依赖编辑器面板元数据。
	return FMath::Clamp(ThresholdPercent, 0, 100);
}

int32 CountConsecutiveThresholdPassingWarmupRuns(
	const TArray<FMonolithWarmupRunRecord>& Runs,
	const FString& ScopeKey,
	const double ThresholdPercent)
{
	if (ScopeKey.IsEmpty())
	{
		return 0;
	}

	int32 ConsecutiveCount = 0;
	for (int32 Index = Runs.Num() - 1; Index >= 0; --Index)
	{
		const FMonolithWarmupRunRecord& Run = Runs[Index];
		if (!Run.ScopeKey.Equals(ScopeKey, ESearchCase::IgnoreCase))
		{
			// 其它 scope 的记录不打断 streak，只是跳过。
			continue;
		}

		if (Run.AttemptedPackages > 0 && Run.CacheHitRatePercent + UE_DOUBLE_SMALL_NUMBER >= ThresholdPercent)
		{
			++ConsecutiveCount;
			continue;
		}

		break;
	}

	return ConsecutiveCount;
}
