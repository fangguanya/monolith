#pragma once

#include "CoreMinimal.h"

/*
 * 在 Level Editor 工具栏上挂 Monolith 入口。
 *
 * 用户痛点：之前的 AssetVisual / 老 indexer 都靠在 console 输 `Monolith.X` 命令，
 * 工具栏才是 editor 工作流的正经入口。这里挂两个 dropdown：
 *
 *  - 「Monolith Build」：选 cohort（Geometric / Semantic / 全部 + Niagara/Anim 待加）
 *      触发 MaterializeAssetVisualFromCache（resume + skip-list + GC 内置）；
 *      旁边再挂 Old indexer Full / Incremental 两个 entry，对应 Monolith.StartIndex full|incremental。
 *
 *  - 「Monolith Cleanup」：每个 cohort 一个清空 entry，调 ClearAssetVisualEntries 把 SQLite 行清空。
 *
 * 多进程并行：Build 路径未来扩展成 spawn 子 editor 进程跑 shard，这里先留 hook。
 */
namespace MonolithToolbar
{
	/** 在 LevelEditor 工具栏注册 Monolith 入口；StartupModule 调一次即可。 */
	void RegisterToolbarEntries();

	/** 反注册；ShutdownModule 调。 */
	void UnregisterToolbarEntries();
}
