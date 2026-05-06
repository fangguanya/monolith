#include "MonolithToolbar.h"

#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "MonolithIndexSubsystem.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MonolithToolbar"

namespace MonolithToolbar
{
	static const FName kBuildSectionName = TEXT("MonolithBuild");
	static const FName kCleanupSectionName = TEXT("MonolithCleanup");
	static const FName kIndexSectionName = TEXT("MonolithIndex");

	/** 弹一个右下角通知，避免 console-only 反馈；用户能立刻看到点了什么。 */
	static void ShowNotification(const FText& Message, bool bIsError = false)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 5.0f;
		Info.bUseSuccessFailIcons = true;
		const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
		}
	}

	static UMonolithIndexSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	}

	/** 触发完整 AssetVisual materialize（resume + skip-list 全自动）。
	 *  这是 GT 同步循环，编辑器会冻结一段时间；我们至少打个 notification 让用户知道开始了。 */
	static void OnMaterializeAllAssetVisualClicked()
	{
		UMonolithIndexSubsystem* Sub = GetSubsystem();
		if (!Sub)
		{
			ShowNotification(LOCTEXT("SubsystemUnavailable", "MonolithIndexSubsystem 不可用"), /*bIsError=*/true);
			return;
		}
		ShowNotification(LOCTEXT("MaterializeAssetVisualStarted", "AssetVisual materialize 已开始（GT 同步循环，可能冻结编辑器几十分钟）"));
		Sub->MaterializeAssetVisualFromCache();
		ShowNotification(LOCTEXT("MaterializeAssetVisualDone", "AssetVisual materialize 完成"));
	}

	/** 并行 build：父进程 spawn N 个 child editor，每个跱 shard 写 DDC，父进程最后 merge SQLite。 */
	static void OnMaterializeParallelClicked(int32 NumWorkers)
	{
		UMonolithIndexSubsystem* Sub = GetSubsystem();
		if (!Sub)
		{
			ShowNotification(LOCTEXT("SubsystemUnavailable", "MonolithIndexSubsystem 不可用"), /*bIsError=*/true);
			return;
		}
		ShowNotification(FText::Format(
			LOCTEXT("ParallelStarted", "并行 build 已启动（{0} 个 child editor）；父编辑器全程冻结，请耐心等待"),
			FText::AsNumber(NumWorkers)));
		Sub->MaterializeAssetVisualParallel(NumWorkers);
		ShowNotification(LOCTEXT("ParallelDone", "并行 build 完成"));
	}

	/** 单 cohort 清空一行：删 SQLite 表 + 清 indexed_asset_metadata 让下次 materialize 重做。 */
	static void OnClearCohortClicked(FName CohortId)
	{
		UMonolithIndexSubsystem* Sub = GetSubsystem();
		if (!Sub)
		{
			ShowNotification(LOCTEXT("SubsystemUnavailable", "MonolithIndexSubsystem 不可用"), /*bIsError=*/true);
			return;
		}
		const bool bOk = Sub->ClearAssetVisualCohort(CohortId);
		ShowNotification(
			FText::Format(LOCTEXT("ClearCohortDone", "已清空 cohort {0}: {1}"),
				FText::FromName(CohortId),
				bOk ? LOCTEXT("OK", "OK") : LOCTEXT("Fail", "FAIL")),
			!bOk);
	}

	/** 触发老 indexer full index（manual-only，跟 Monolith.StartIndex full 等价）。 */
	static void OnFullIndexClicked()
	{
		UMonolithIndexSubsystem* Sub = GetSubsystem();
		if (!Sub)
		{
			ShowNotification(LOCTEXT("SubsystemUnavailable", "MonolithIndexSubsystem 不可用"), /*bIsError=*/true);
			return;
		}
		Sub->RequestFullIndexFromUI();
		ShowNotification(LOCTEXT("FullIndexStarted", "Full index 已请求"));
	}

	static void OnIncrementalIndexClicked()
	{
		UMonolithIndexSubsystem* Sub = GetSubsystem();
		if (!Sub)
		{
			ShowNotification(LOCTEXT("SubsystemUnavailable", "MonolithIndexSubsystem 不可用"), /*bIsError=*/true);
			return;
		}
		if (!Sub->CanDoIncrementalIndexFromUI())
		{
			ShowNotification(LOCTEXT("NoBaseline", "没有 incremental baseline，先跑一次 Full index"), /*bIsError=*/true);
			return;
		}
		Sub->StartIncrementalIndexFromUI();
		ShowNotification(LOCTEXT("IncrementalStarted", "Incremental index 已启动"));
	}

	/** 单 dropdown，按 section 分组：Build（构建）/ Old Indexer / Cleanup（清空）。 */
	static TSharedRef<SWidget> MakeMonolithDropdown()
	{
		FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

		MenuBuilder.BeginSection(NAME_None, LOCTEXT("BuildAssetVisualSection", "构建 AssetVisual (resume + skip-list 自动)"));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("BuildAll", "Materialize 全部 cohort (单进程)"),
			LOCTEXT("BuildAllTip", "扫所有支持类资产，未完成的 build artifact + 写 DDC + 写 SQLite。已完成的自动跳过。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnMaterializeAllAssetVisualClicked)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("BuildParallel4", "Materialize 全部 (并行 4 进程)"),
			LOCTEXT("BuildParallel4Tip", "spawn 4 个 child editor 各跱 1/4 shard，写 DDC；父进程最后 merge 到 SQLite。预计单进程 1/4 时间。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnMaterializeParallelClicked, 4)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("BuildParallel8", "Materialize 全部 (并行 8 进程)"),
			LOCTEXT("BuildParallel8Tip", "spawn 8 个 child editor 各跱 1/8 shard。仅在显存够 / 内存够时使用（每 child 约 8GB）。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnMaterializeParallelClicked, 8)));
		MenuBuilder.EndSection();

		MenuBuilder.BeginSection(NAME_None, LOCTEXT("BuildOldIndexerSection", "传统 Indexer (Blueprint / Material / Mesh / 等)"));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("FullIndex", "Full Index"),
			LOCTEXT("FullIndexTip", "等价 Monolith.StartIndex full：全量重建索引，去掉所有过期数据。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnFullIndexClicked)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("IncrementalIndex", "Incremental Index"),
			LOCTEXT("IncrementalIndexTip", "等价 Monolith.StartIndex incremental：基于上次 baseline 增量更新。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnIncrementalIndexClicked)));
		MenuBuilder.EndSection();

		MenuBuilder.BeginSection(NAME_None, LOCTEXT("CleanupAssetVisualSection", "清空 (按 cohort 单独清空 SQLite 行)"));

		const FName GeoId(TEXT("AssetVisualGeometric"));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ClearGeo", "清空 AssetVisualGeometric"),
			LOCTEXT("ClearGeoTip", "删 SQLite asset_visual_geometric 表全部行；下次 Materialize 会重做。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnClearCohortClicked, GeoId)));

		const FName SemId(TEXT("AssetVisualSemantic"));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ClearSem", "清空 AssetVisualSemantic"),
			LOCTEXT("ClearSemTip", "删 SQLite asset_visual_semantic 表全部行；下次 Materialize 会重做（CLIP 推理）。"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&OnClearCohortClicked, SemId)));

		MenuBuilder.EndSection();
		return MenuBuilder.MakeWidget();
	}

	void RegisterToolbarEntries()
	{
		UToolMenus* Menus = UToolMenus::Get();
		if (!Menus)
		{
			return;
		}

		// 同 MonolithIndexModule.cpp 已有的 "Full Index" 按钮放在一起：AssetsToolBar，紧挨 Content section。
		UToolMenu* Toolbar = Menus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.AssetsToolBar"));
		if (!Toolbar)
		{
			UE_LOG(LogTemp, Warning, TEXT("MonolithToolbar: AssetsToolBar 不存在，工具栏入口无法注册"));
			return;
		}

		FToolMenuSection& Section = Toolbar->FindOrAddSection(
			kBuildSectionName,
			FText::GetEmpty(),
			FToolMenuInsert(TEXT("MonolithIndexToolbar"), EToolMenuInsertType::After));

		// 用 InitWidget + 自建 SComboButton 而不是 InitComboButton —— LevelEditor.AssetsToolBar 默认 style
		// 会把 InitComboButton 折叠成纯 chevron（label 被吞），自建按钮可强制显示文字。
		// 三个 section（构建 AssetVisual / 传统 indexer / 清空）通过 OnGetMenuContent 拼出来。
		Section.AddEntry(FToolMenuEntry::InitWidget(
			TEXT("MonolithCombo"),
			SNew(SBox)
			.Padding(FMargin(4.0f, 0.0f))
			[
				SNew(SComboButton)
				.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
				.HasDownArrow(true)
				.ToolTipText(LOCTEXT("MonolithTip", "Monolith 索引：构建 / 维护 / 清空"))
				.OnGetMenuContent(FOnGetContent::CreateStatic(&MakeMonolithDropdown))
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MonolithLabel", "Monolith Index"))
				]
			],
			LOCTEXT("MonolithLabel", "Monolith Index"),
			/*bNoIndent=*/true,
			/*bSearchable=*/false));
	}

	void UnregisterToolbarEntries()
	{
		UToolMenus* Menus = UToolMenus::Get();
		if (!Menus)
		{
			return;
		}
		Menus->UnregisterOwnerByName(kBuildSectionName);
		Menus->UnregisterOwnerByName(kCleanupSectionName);
	}
}

#undef LOCTEXT_NAMESPACE
