#pragma once

#include "CoreMinimal.h"
#include "MonolithIndexSubsystem.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SComboButton;
class SWidget;

/*
 * 这是编辑器底部状态栏里那个 Monolith Index 小挂件。
 *
 * 它会定时从子系统拿两份快照：
 * - SummarySnapshot：更新频率更高，负责一行摘要；
 * - DetailSnapshot：更新稍慢，负责展开菜单里的详细信息。
 */
class SMonolithIndexStatusBarWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMonolithIndexStatusBarWidget) {}
	SLATE_END_ARGS()

	/** 构建状态栏控件。 */
	void Construct(const FArguments& InArgs);

private:
	/** 生成点击后弹出的详细菜单。 */
	TSharedRef<SWidget> CreateStatusBarMenu();
	/** 定时刷新摘要快照。 */
	EActiveTimerReturnType UpdateSummarySnapshot(double InCurrentTime, float InDeltaTime);
	/** 定时刷新明细快照。 */
	EActiveTimerReturnType UpdateDetailSnapshot(double InCurrentTime, float InDeltaTime);
	/** 展开/关闭菜单时同步明细。 */
	void HandleMenuOpenChanged(bool bIsOpen);

	/** 状态栏主文本。 */
	FText GetSummaryText() const;
	/** 鼠标悬停时的摘要提示。 */
	FText GetSummaryToolTipText() const;
	/** 左侧小圆点的状态颜色。 */
	FSlateColor GetIndicatorColor() const;

	/** 菜单里的“总状态”一行。 */
	FText GetDetailStatusLine() const;
	/** 菜单里的“队列/进度”一行。 */
	FText GetDetailQueueLine() const;
	/** 菜单里的“读取命中率”一行。 */
	FText GetDetailReadLine() const;
	/** 菜单里的“写入统计”一行。 */
	FText GetDetailWriteLine() const;
	/** 菜单里的“GT breaker”一行。 */
	FText GetDetailGtLine() const;
	/** 菜单里的“远端 breaker”一行。 */
	FText GetDetailRemoteLine() const;

	/** 主动刷新摘要快照。 */
	void RefreshSummarySnapshot();
	/** 主动刷新详细快照。 */
	void RefreshDetailSnapshot();
	/** 取编辑器里的 Monolith 子系统。 */
	UMonolithIndexSubsystem* GetSubsystem() const;

	/** 负责承载下拉菜单的按钮。 */
	TSharedPtr<SComboButton> ComboButton;
	/** 用于状态栏一行摘要的快照。 */
	FMonolithIndexStatusBarSnapshot SummarySnapshot;
	/** 用于展开菜单详情的快照。 */
	FMonolithIndexStatusBarSnapshot DetailSnapshot;
};
