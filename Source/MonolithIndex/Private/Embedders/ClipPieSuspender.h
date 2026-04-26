#pragma once

#include "CoreMinimal.h"

/*
 * Semantic indexer 的 PIE 暂停器。
 *
 * 设计动机：CLIP-ViT-B/32 推理在 4070 上单 batch 占用 ~800MB VRAM；PIE 时的 game world
 * 渲染随时可能瞬间吃掉剩余 11GB 中的大半。两者并发会触发 OOM 或 driver reset。
 *
 * 行为：
 *  - 监听 FEditorDelegates::BeginPIE / EndPIE
 *  - PIE 中 IsPaused() 返回 true，semantic indexer 自己在 BuildArtifact 入口检查后直接返回 false
 *  - PIE 退出时自动恢复
 *
 * geometric indexer 完全不依赖 GPU，不受此限制。
 */
class FClipPieSuspender
{
public:
	/** 注册 PIE 委托；可在模块启动时调用一次。 */
	void Register();

	/** 反注册委托；模块关闭时调用。 */
	void Unregister();

	/** 当前是否处于 PIE-暂停态。 */
	bool IsPaused() const;

private:
	void HandleBeginPie(const bool bInIsSimulating);
	void HandleEndPie(const bool bInIsSimulating);

	mutable FCriticalSection Mutex;
	bool bIsPaused = false;
	FDelegateHandle BeginPieHandle;
	FDelegateHandle EndPieHandle;
};
