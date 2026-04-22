#include "MonolithIndexModule.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"
#include "Actions/ProjectSearchAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Actions/ProjectGetStatsAction.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectListStalePackagesAction.h"
#include "Actions/ProjectListGameplayTagsAction.h"
#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "SMonolithIndexStatusBar.h"
#include "ToolMenus.h"

/*
 * 这个文件是 MonolithIndex 模块的“门厅”。
 *
 * 进入点很少，但很重要：
 * - StartupModule 负责把 MCP action 一口气注册完；
 * - RegisterMenus 负责把状态栏 UI 挂进编辑器；
 * - ShutdownModule 负责把这些入口安全拆掉。
 *
 * 这里不做重业务计算，重点是“把入口接好、关干净”。
 */

#define LOCTEXT_NAMESPACE "FMonolithIndexModule"

void FMonolithIndexModule::StartupModule()
{
	UE_LOG(LogMonolithIndex, Verbose, TEXT("Monolith -- Index module loaded (8 actions, SQLite+FTS5)"));

	// ToolRegistry 像一张“动作通讯录”。
	// 每注册一个 action，外部就多了一个可以调用的 MCP 工具入口。
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("project"), FProjectSearchAction::GetName(),
		FProjectSearchAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectSearchAction::Execute),
		FProjectSearchAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectFindReferencesAction::GetName(),
		FProjectFindReferencesAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectFindReferencesAction::Execute),
		FProjectFindReferencesAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectFindByTypeAction::GetName(),
		FProjectFindByTypeAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectFindByTypeAction::Execute),
		FProjectFindByTypeAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectGetStatsAction::GetName(),
		FProjectGetStatsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectGetStatsAction::Execute),
		FProjectGetStatsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectGetAssetDetailsAction::GetName(),
		FProjectGetAssetDetailsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectGetAssetDetailsAction::Execute),
		FProjectGetAssetDetailsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectListStalePackagesAction::GetName(),
		FProjectListStalePackagesAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectListStalePackagesAction::Execute),
		FProjectListStalePackagesAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectListGameplayTagsAction::GetName(),
		FProjectListGameplayTagsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectListGameplayTagsAction::Execute),
		FProjectListGameplayTagsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectSearchGameplayTagsAction::GetName(),
		FProjectSearchGameplayTagsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectSearchGameplayTagsAction::Execute),
		FProjectSearchGameplayTagsAction::GetSchema());

	if (!IsRunningCommandlet())
	{
		// 只有真正的编辑器界面模式才需要注册菜单。
		// commandlet/headless 模式下没有工具栏，也不该去碰 UI。
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMonolithIndexModule::RegisterMenus));
	}
}

void FMonolithIndexModule::ShutdownModule()
{
	// 先拆 UI，再拆 action 命名空间，避免编辑器还持有悬空入口。
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("project"));
}

void FMonolithIndexModule::RegisterMenus()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	// 确保 LevelEditor 已经可用，否则状态栏菜单还不存在。
	FModuleManager::Get().LoadModule(TEXT("LevelEditor"));

	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* const Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.StatusBar.ToolBar");
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->AddSection(
		TEXT("MonolithIndex"),
		FText::GetEmpty(),
		FToolMenuInsert(TEXT("Compile"), EToolMenuInsertType::Before));

	// 这里放的是一个持续刷新的状态小部件，
	// 目的是让用户不用打开日志也能看到索引是否在忙、进度到哪了。
	Section.AddEntry(
		FToolMenuEntry::InitWidget(
			TEXT("MonolithIndexStatusBar"),
			SNew(SMonolithIndexStatusBarWidget),
			LOCTEXT("MonolithIndexStatusBarLabel", "Monolith Index"),
			true,
			false));

	UE_LOG(LogMonolithIndex, Log, TEXT("Registered MonolithIndex status bar widget in LevelEditor.StatusBar.ToolBar"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithIndexModule, MonolithIndex)
