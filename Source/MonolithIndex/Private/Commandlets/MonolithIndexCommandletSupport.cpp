#include "Commandlets/MonolithIndexCommandletSupport.h"

#include "HAL/PlatformTime.h"
#include "Misc/Parse.h"

bool FMonolithWarmupCommandletArgs::ShouldStopForTimeWindow(const double StartSeconds, const double NowSeconds) const
{
	// TimeWindowMinutes <= 0 约定为“不设上限”。
	if (TimeWindowMinutes <= 0)
	{
		return false;
	}

	// 用分钟来比较，是为了和命令行参数保持同一单位。
	const double ElapsedMinutes = (NowSeconds - StartSeconds) / 60.0;
	return ElapsedMinutes >= static_cast<double>(TimeWindowMinutes);
}

bool ParseMonolithWarmupCommandletArgs(
	const FString& Params,
	FMonolithWarmupCommandletArgs& OutArgs,
	FString& OutError)
{
	OutArgs = FMonolithWarmupCommandletArgs();
	OutError.Empty();

	FString ScopeValue;
	if (FParse::Value(*Params, TEXT("Scope="), ScopeValue))
	{
		// Scope 是 warmup 最关键的分流参数，先单独完整解析。
		if (ScopeValue.Equals(TEXT("OfflineOnly"), ESearchCase::IgnoreCase))
		{
			OutArgs.Scope.Kind = EMonolithWarmupScopeKind::OfflineOnly;
		}
		else if (ScopeValue.Equals(TEXT("All"), ESearchCase::IgnoreCase))
		{
			OutArgs.Scope.Kind = EMonolithWarmupScopeKind::All;
		}
		else if (ScopeValue.Equals(TEXT("GlobalReducer"), ESearchCase::IgnoreCase))
		{
			OutArgs.Scope.Kind = EMonolithWarmupScopeKind::GlobalReducer;
		}
		else if (ScopeValue.StartsWith(TEXT("Cohort:"), ESearchCase::IgnoreCase))
		{
			// `Cohort:Blueprint` 这种形式需要把冒号后面的 cohort 名截出来。
			const FString CohortName = ScopeValue.RightChop(7);
			if (CohortName.IsEmpty())
			{
				OutError = TEXT("Scope=Cohort:<Name> 缺少 cohort 名称");
				return false;
			}
			OutArgs.Scope.Kind = EMonolithWarmupScopeKind::Cohort;
			OutArgs.Scope.CohortName = FName(*CohortName);
		}
		else
		{
			OutError = FString::Printf(TEXT("不支持的 Scope 参数: %s"), *ScopeValue);
			return false;
		}
	}

	// 其它参数都是简单标量，直接复用 Parse::Value 即可。
	FParse::Value(*Params, TEXT("Priority="), OutArgs.Priority);
	FParse::Value(*Params, TEXT("TimeWindowMinutes="), OutArgs.TimeWindowMinutes);
	FParse::Value(*Params, TEXT("MaxPackages="), OutArgs.MaxPackages);

	// 负数没有意义，这里直接钳成 0，避免下游再重复防御。
	OutArgs.TimeWindowMinutes = FMath::Max(0, OutArgs.TimeWindowMinutes);
	OutArgs.MaxPackages = FMath::Max(0, OutArgs.MaxPackages);
	if (OutArgs.Priority.IsEmpty())
	{
		// priority 缺失时给一个保守默认值，避免命令行使用者忘写。
		OutArgs.Priority = TEXT("Background");
	}

	return true;
}

bool DoesMonolithWarmupScopeTargetIndexer(
	const FMonolithWarmupScope& Scope,
	const FName& IndexerId,
	const FString& IndexerName,
	const EMonolithExecutionMode ExecutionMode)
{
	switch (Scope.Kind)
	{
	case EMonolithWarmupScopeKind::OfflineOnly:
		return false;
	case EMonolithWarmupScopeKind::All:
		return true;
	case EMonolithWarmupScopeKind::GlobalReducer:
		return ExecutionMode == EMonolithExecutionMode::GlobalReducer;
	case EMonolithWarmupScopeKind::Cohort:
	default:
		// cohort 既支持更稳定的 indexer id，也兼容人类更容易输入的显示名。
		return Scope.CohortName == IndexerId
			|| Scope.CohortName.ToString().Equals(IndexerName, ESearchCase::IgnoreCase);
	}
}

FString GetRunningMonolithCommandletNameFromCommandLine(const FString& CommandLine)
{
	// Unreal 命令行里 commandlet 名通常挂在 `run=` 后面。
	FString CommandletName;
	FParse::Value(*CommandLine, TEXT("run="), CommandletName);
	CommandletName.TrimQuotesInline();
	return CommandletName;
}

bool ShouldMonolithCommandletBypassLocalSqlite(const FString& CommandletName)
{
	// 这里集中列出需要“绕过本地 SQLite 常规路径”的 commandlet 名单。
	// 这样以后新增命令时，只改这一处就够了。
	return CommandletName.Equals(TEXT("MonolithIndexWarmup"), ESearchCase::IgnoreCase)
		|| CommandletName.Equals(TEXT("MonolithIdentityPoc"), ESearchCase::IgnoreCase)
		|| CommandletName.EndsWith(TEXT("MonolithIndexWarmup"), ESearchCase::IgnoreCase)
		|| CommandletName.EndsWith(TEXT("MonolithIdentityPoc"), ESearchCase::IgnoreCase);
}
