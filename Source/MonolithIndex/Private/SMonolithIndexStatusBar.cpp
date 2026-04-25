#include "SMonolithIndexStatusBar.h"

#include "Editor.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateColor.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

/*
 * 这个文件负责把一堆统计数字翻译成人更容易扫读的状态栏文案。
 *
 * 目标不是完整展示所有原始数据，
 * 而是让人一眼看出：
 * - 索引现在忙不忙；
 * - 本地/远端 cache 命中情况如何；
 * - GT breaker 和远端 breaker 有没有在报警。
 */
namespace MonolithIndexStatusBarInternal
{
	/** 总读取次数 = 本地命中 + 远端命中 + 远端 miss。 */
	static uint64 GetReadTotal(const FMonolithIndexStatusBarSnapshot& Snapshot)
	{
		return Snapshot.LocalHitCount + Snapshot.RemoteHitCount + Snapshot.RemoteMissCount;
	}

	/** 远端读取次数 = 远端命中 + 远端 miss。 */
	static uint64 GetRemoteReadTotal(const FMonolithIndexStatusBarSnapshot& Snapshot)
	{
		return Snapshot.RemoteHitCount + Snapshot.RemoteMissCount;
	}

	/** 把分子/分母格式化成整数字百分比。 */
	static FString FormatPercent(const uint64 Numerator, const uint64 Denominator)
	{
		if (Denominator == 0)
		{
			return TEXT("0%");
		}

		return FString::Printf(TEXT("%.0f%%"), 100.0 * static_cast<double>(Numerator) / static_cast<double>(Denominator));
	}

	/** 把秒数压成简短文本，比如 12s / 4m / 1.2h。 */
	static FString FormatDurationShort(const double Seconds)
	{
		if (Seconds <= 0.0)
		{
			return TEXT("0s");
		}

		if (Seconds < 60.0)
		{
			return FString::Printf(TEXT("%.0fs"), FMath::RoundToDouble(Seconds));
		}

		const double Minutes = Seconds / 60.0;
		if (Minutes < 60.0)
		{
			return FString::Printf(TEXT("%.0fm"), FMath::RoundToDouble(Minutes));
		}

		const double Hours = Minutes / 60.0;
		return FString::Printf(TEXT("%.1fh"), Hours);
	}

	/** 字节数转成 MB 文本。 */
	static FString FormatMegabytes(const uint64 Bytes)
	{
		return FString::Printf(TEXT("%.1f MB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
	}

	/** breaker 状态统一显示成 ready 或 open XXs。 */
	static FString FormatBreakerState(const bool bOpen, const double RemainingSeconds)
	{
		return bOpen ? FString::Printf(TEXT("open %s"), *FormatDurationShort(RemainingSeconds)) : TEXT("ready");
	}

	/** 本地 cache 只要对象还活着，就认为本地层是 ready。 */
	static FString FormatLocalCacheState(const bool bAvailable)
	{
		return bAvailable ? TEXT("ready") : TEXT("unavailable");
	}

	/** 生成状态栏里最短那一行摘要文本。 */
	static FString FormatSummary(const FMonolithIndexStatusBarSnapshot& Snapshot)
	{
		if (!Snapshot.bDatabaseOpen)
		{
			return TEXT("Monolith unavailable");
		}

		FString Summary;
		if (!Snapshot.bIndexEnabled)
		{
			Summary = TEXT("Query only");
		}
		else if (Snapshot.bIndexingInProgress)
		{
			if (Snapshot.TotalItems > 0)
			{
				Summary = FString::Printf(TEXT("Index %d/%d"), Snapshot.CompletedItems, Snapshot.TotalItems);
			}
			else
			{
				Summary = TEXT("Indexing");
			}

			if (Snapshot.RemainingItems > 0)
			{
				Summary += FString::Printf(TEXT(" | left %s"), *FormatDurationShort(Snapshot.EtaSeconds));
			}
		}
		else
		{
			Summary = TEXT("Index idle");
		}

		const uint64 ReadTotal = GetReadTotal(Snapshot);
		Summary += FString::Printf(
			TEXT(" | local %s | remote %s | miss %s | write %s | sync %d/%d | GT %s | remote %s"),
			*FormatPercent(Snapshot.LocalHitCount, ReadTotal),
			*FormatPercent(Snapshot.RemoteHitCount, ReadTotal),
			*FormatPercent(Snapshot.RemoteMissCount, ReadTotal),
			*FormatMegabytes(Snapshot.RemoteWriteBytes),
			Snapshot.PendingRemoteWriteCount,
			Snapshot.InFlightRemoteWriteCount,
			*FormatBreakerState(Snapshot.bGtBreakerOpen, Snapshot.GtBreakerRemainingSeconds),
			*FormatBreakerState(Snapshot.bRemoteDisabled, Snapshot.RemoteBreakerRemainingSeconds));
		return Summary;
	}

	/** 生成菜单详情顶部的状态文字。 */
	static FString FormatStatusText(const FMonolithIndexStatusBarSnapshot& Snapshot)
	{
		if (!Snapshot.bDatabaseOpen)
		{
			return TEXT("本地 SQLite 未打开");
		}
		if (!Snapshot.bIndexEnabled)
		{
			return TEXT("索引已关闭，当前为 query-only 模式");
		}
		if (!Snapshot.StatusMessage.IsEmpty())
		{
			return Snapshot.StatusMessage;
		}
		return Snapshot.bIndexingInProgress ? TEXT("索引处理中") : TEXT("索引空闲");
	}
}

void SMonolithIndexStatusBarWidget::Construct(const FArguments& InArgs)
{
	// 先拿一次快照，避免控件刚出现时显示默认空值。
	RefreshSummarySnapshot();
	DetailSnapshot = SummarySnapshot;

	this->ChildSlot
	[
		SAssignNew(ComboButton, SComboButton)
		.ContentPadding(FMargin(6.0f, 0.0f))
		.MenuPlacement(MenuPlacement_AboveAnchor)
		.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
		.OnMenuOpenChanged(this, &SMonolithIndexStatusBarWidget::HandleMenuOpenChanged)
		.OnGetMenuContent(this, &SMonolithIndexStatusBarWidget::CreateStatusBarMenu)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(this, &SMonolithIndexStatusBarWidget::GetIndicatorColor)
				.Padding(0.0f)
				[
					SNew(SSpacer)
					.Size(FVector2D(8.0f, 8.0f))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SMonolithIndexStatusBarWidget::GetSummaryText)
				.ToolTipText(this, &SMonolithIndexStatusBarWidget::GetSummaryToolTipText)
			]
		]
	];

	RegisterActiveTimer(0.2f, FWidgetActiveTimerDelegate::CreateSP(this, &SMonolithIndexStatusBarWidget::UpdateSummarySnapshot));
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SMonolithIndexStatusBarWidget::UpdateDetailSnapshot));
}

TSharedRef<SWidget> SMonolithIndexStatusBarWidget::CreateStatusBarMenu()
{
	return SNew(SBox)
		.WidthOverride(420.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Monolith Index")))
					.Font(FAppStyle::GetFontStyle(TEXT("BoldFont")))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailStatusLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailQueueLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailReadLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailLocalCacheLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailWriteLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailRemoteLine)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SMonolithIndexStatusBarWidget::GetDetailGtLine)
				]
			]
		];
}

EActiveTimerReturnType SMonolithIndexStatusBarWidget::UpdateSummarySnapshot(double InCurrentTime, float InDeltaTime)
{
	RefreshSummarySnapshot();
	return EActiveTimerReturnType::Continue;
}

EActiveTimerReturnType SMonolithIndexStatusBarWidget::UpdateDetailSnapshot(double InCurrentTime, float InDeltaTime)
{
	if (ComboButton.IsValid() && ComboButton->IsOpen())
	{
		RefreshDetailSnapshot();
	}
	return EActiveTimerReturnType::Continue;
}

void SMonolithIndexStatusBarWidget::HandleMenuOpenChanged(const bool bIsOpen)
{
	if (bIsOpen)
	{
		RefreshDetailSnapshot();
	}
}

FText SMonolithIndexStatusBarWidget::GetSummaryText() const
{
	return FText::FromString(MonolithIndexStatusBarInternal::FormatSummary(SummarySnapshot));
}

FText SMonolithIndexStatusBarWidget::GetSummaryToolTipText() const
{
	return FText::FromString(MonolithIndexStatusBarInternal::FormatSummary(DetailSnapshot));
}

FSlateColor SMonolithIndexStatusBarWidget::GetIndicatorColor() const
{
	if (!SummarySnapshot.bDatabaseOpen)
	{
		return FLinearColor(0.45f, 0.45f, 0.45f);
	}
	if (SummarySnapshot.bGtBreakerOpen || SummarySnapshot.bRemoteDisabled)
	{
		return FLinearColor(0.85f, 0.2f, 0.2f);
	}
	if (SummarySnapshot.bIndexingInProgress)
	{
		return FLinearColor(0.25f, 0.55f, 0.95f);
	}
	return FLinearColor(0.2f, 0.7f, 0.35f);
}

FText SMonolithIndexStatusBarWidget::GetDetailStatusLine() const
{
	return FText::FromString(FString::Printf(
		TEXT("状态: %s"),
		*MonolithIndexStatusBarInternal::FormatStatusText(DetailSnapshot)));
}

FText SMonolithIndexStatusBarWidget::GetDetailQueueLine() const
{
	const FString StaleText = DetailSnapshot.StalePackageCount == INDEX_NONE
		? TEXT("refreshing")
		: FString::FromInt(DetailSnapshot.StalePackageCount);
	const FString OfflineText = DetailSnapshot.OfflineQueueDepth == INDEX_NONE
		? TEXT("refreshing")
		: FString::FromInt(DetailSnapshot.OfflineQueueDepth);

	return FText::FromString(FString::Printf(
		TEXT("队列: %d/%d, remaining=%d, eta=%s, stale=%s, offline=%s"),
		DetailSnapshot.CompletedItems,
		DetailSnapshot.TotalItems,
		DetailSnapshot.RemainingItems,
		*MonolithIndexStatusBarInternal::FormatDurationShort(DetailSnapshot.EtaSeconds),
		*StaleText,
		*OfflineText));
}

FText SMonolithIndexStatusBarWidget::GetDetailReadLine() const
{
	const uint64 ReadTotal = MonolithIndexStatusBarInternal::GetReadTotal(DetailSnapshot);
	return FText::FromString(FString::Printf(
		TEXT("本次启动读取: total=%llu, local=%llu (%s), remote=%llu (%s), miss=%llu (%s)"),
		ReadTotal,
		DetailSnapshot.LocalHitCount,
		*MonolithIndexStatusBarInternal::FormatPercent(DetailSnapshot.LocalHitCount, ReadTotal),
		DetailSnapshot.RemoteHitCount,
		*MonolithIndexStatusBarInternal::FormatPercent(DetailSnapshot.RemoteHitCount, ReadTotal),
		DetailSnapshot.RemoteMissCount,
		*MonolithIndexStatusBarInternal::FormatPercent(DetailSnapshot.RemoteMissCount, ReadTotal)));
}

FText SMonolithIndexStatusBarWidget::GetDetailLocalCacheLine() const
{
	const uint64 ReadTotal = MonolithIndexStatusBarInternal::GetReadTotal(DetailSnapshot);
	return FText::FromString(FString::Printf(
		TEXT("本地缓存: state=%s, reused=%llu, read_total=%llu"),
		*MonolithIndexStatusBarInternal::FormatLocalCacheState(DetailSnapshot.bLocalCacheAvailable),
		DetailSnapshot.LocalHitCount,
		ReadTotal));
}

FText SMonolithIndexStatusBarWidget::GetDetailWriteLine() const
{
	return FText::FromString(FString::Printf(
		TEXT("远端写入: ok=%llu, fail=%llu, pending=%d, inflight=%d, encoded=%s, oversized=%llu"),
		DetailSnapshot.RemoteWriteOkCount,
		DetailSnapshot.RemoteWriteFailCount,
		DetailSnapshot.PendingRemoteWriteCount,
		DetailSnapshot.InFlightRemoteWriteCount,
		*MonolithIndexStatusBarInternal::FormatMegabytes(DetailSnapshot.RemoteWriteBytes),
		DetailSnapshot.OversizedArtifactCount));
}

FText SMonolithIndexStatusBarWidget::GetDetailGtLine() const
{
	return FText::FromString(FString::Printf(
		TEXT("GT: overrun=%llu, downgraded=%llu, breaker=%s"),
		DetailSnapshot.GtOverrunCount,
		DetailSnapshot.GtDowngradeCount,
		*MonolithIndexStatusBarInternal::FormatBreakerState(
			DetailSnapshot.bGtBreakerOpen,
			DetailSnapshot.GtBreakerRemainingSeconds)));
}

FText SMonolithIndexStatusBarWidget::GetDetailRemoteLine() const
{
	const uint64 RemoteReadTotal = MonolithIndexStatusBarInternal::GetRemoteReadTotal(DetailSnapshot);
	return FText::FromString(FString::Printf(
		TEXT("远端缓存: state=%s, read_total=%llu, hit=%llu, miss=%llu, sync=%d/%d"),
		*MonolithIndexStatusBarInternal::FormatBreakerState(
			DetailSnapshot.bRemoteDisabled,
			DetailSnapshot.RemoteBreakerRemainingSeconds),
		RemoteReadTotal,
		DetailSnapshot.RemoteHitCount,
		DetailSnapshot.RemoteMissCount,
		DetailSnapshot.PendingRemoteWriteCount,
		DetailSnapshot.InFlightRemoteWriteCount));
}

void SMonolithIndexStatusBarWidget::RefreshSummarySnapshot()
{
	if (UMonolithIndexSubsystem* const Subsystem = GetSubsystem())
	{
		SummarySnapshot = Subsystem->GetStatusBarSnapshot(false);
	}
	else
	{
		SummarySnapshot = FMonolithIndexStatusBarSnapshot();
	}
}

void SMonolithIndexStatusBarWidget::RefreshDetailSnapshot()
{
	if (UMonolithIndexSubsystem* const Subsystem = GetSubsystem())
	{
		DetailSnapshot = Subsystem->GetStatusBarSnapshot(true);
	}
	else
	{
		DetailSnapshot = FMonolithIndexStatusBarSnapshot();
	}
}

UMonolithIndexSubsystem* SMonolithIndexStatusBarWidget::GetSubsystem() const
{
	return GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
}
