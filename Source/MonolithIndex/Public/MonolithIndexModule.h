#pragma once

#include "Modules/ModuleManager.h"

/*
 * FMonolithIndexModule 是 MonolithIndex 这个 UE 模块的入口。
 *
 * 它主要做两件事：
 * 1. 启动时把 project.* 这批 MCP action 注册出去；
 * 2. 在编辑器模式下把状态栏小部件和顶部索引按钮挂到 LevelEditor 的工具条上。
 *
 * 真正的索引逻辑不在这里跑。
 * 这个模块更像“接线员”，负责把外界入口和 MonolithIndex 子系统接起来。
 */
class FMonolithIndexModule : public IModuleInterface
{
public:
	/** 模块启动时注册 action 和菜单。 */
	virtual void StartupModule() override;
	/** 模块关闭时注销菜单和命名空间。 */
	virtual void ShutdownModule() override;

private:
	/** 把 MonolithIndex 的状态栏和顶部工具栏入口挂到编辑器工具条。 */
	void RegisterMenus();
};
