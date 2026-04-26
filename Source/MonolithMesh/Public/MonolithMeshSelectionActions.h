#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/*
 * Mesh selection / cursor 直达 action 注册入口。
 *
 * 这两个 action 不走视觉 embedding，纯走编辑器上下文：
 *  - mesh.get_selected_mesh_assets：拿当前 EditorWorld selection -> 反查 StaticMesh / SkeletalMesh
 *  - mesh.query_mesh_under_cursor：给屏幕坐标 / 鼠标 -> line trace -> 命中 component -> 底层 mesh
 *
 * spec 硬约束：
 *  - 必须在游戏线程；
 *  - p95 ≤ 5ms (selection) / ≤ 16ms (cursor)；
 *  - HISM/ISM 命中返回 instance_index；
 *  - 多 mesh 组件命中只返回命中那个；
 *  - 非 mesh actor 返回空数组而非 error；
 *  - PIE 默认作用于 EditorWorld；显式 world=pie 才作用于 PIE 世界。
 */
class FMonolithMeshSelectionActions
{
public:
	/** 把两个 action 注册到全局 registry。 */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleGetSelectedMeshAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleQueryMeshUnderCursor(const TSharedPtr<FJsonObject>& Params);
};
