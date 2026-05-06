#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "NiagaraSystem.h"

/*
 * AssetVisual cohort 的"多 phase 索引"配置 —— 同一资产在 cohort 内分几行存放、每行用什么时间点渲染。
 *
 * 设计目标：
 *  - geometric / semantic 两个 indexer 必须看到完全相同的 phase 列表；否则 PhaseId=k 在两 cohort 之间
 *    对应的不是同一时间点，preview PNG 复用、search 端按 phase_id GROUP BY 都会错位。
 *  - 单 phase 资产（StaticMesh/Material/Widget/AnimBlueprint）走默认单元素列表 (PhaseId=0, PhaseT=0)，
 *    保持与历史行为兼容；新增的真正多 phase 资产是 AnimSequence/AnimMontage/AnimComposite/NiagaraSystem。
 */
struct FAssetVisualPhaseDef
{
	/** 稳定整数键；同 asset 内 0..N-1，写到 SQLite 的 phase_id 列。 */
	uint8 PhaseId = 0;
	/*
	 * Phase 采样时间。
	 *
	 *  - Anim 系列：归一化 0..1，渲染时乘以 anim 总时长。
	 *  - Niagara 系列：仿真秒数。
	 *  - 单 phase：0。
	 *
	 * 与 FCanonicalRenderRequest::PhaseT 含义一致；renderer 直接消费这个值。
	 */
	float PhaseT = 0.0f;
	/** 给搜索结果用的可读名（"early"/"peak"/"tail"...）；写到 SQLite 的 phase_label 列。 */
	FString Label;
};

namespace AssetVisualPhase
{
	/*
	 * 判定一个 anim 是否能走 SkelMeshComp + UAnimSingleNodeInstance 的多 phase 渲染旁路。
	 *
	 * 必要条件：拿得到 USkeleton 且能解析出一份 USkeletalMesh（PreviewMesh 或 FindCompatibleMesh）。
	 * 这个判定与 AssetCanonicalRenderer.cpp 里 ResolveAnimPreviewMesh 的逻辑必须保持一致 ——
	 * 二者一旦走偏，要么 GetPhasesForAsset 返回 3 phase 但 renderer 渲不出来 anim 整资产 stale，
	 * 要么 GetPhasesForAsset 返回 1 phase 但 renderer 实际能渲 3 帧浪费容量。
	 */
	inline bool AnimHasPreviewMesh(const UAnimSequenceBase* Anim)
	{
		if (!Anim)
		{
			return false;
		}
		USkeleton* Skeleton = Anim->GetSkeleton();
		if (!Skeleton)
		{
			return false;
		}
		if (Skeleton->GetPreviewMesh())
		{
			return true;
		}
		// PreviewMesh 没设但 Skeleton 自己能找一份 compatible mesh 的话也算通过。
		return Skeleton->FindCompatibleMesh() != nullptr;
	}

	/*
	 * 给某资产返回它在 AssetVisual cohort 内的 phase 列表。
	 *
	 * 当前规则（与 HANDOFF_PHASE_A.md 设计一致）：
	 *  - AnimSequenceBase（含 Sequence / Montage / Composite）：有 preview mesh 时 3 phase = 25% / 50% / 75% 时长
	 *    没 preview mesh 退化为单 phase（renderer 走 thumbnail manager 兜底）
	 *  - NiagaraSystem：3 phase = 0.5s / 1.5s / 3.0s 仿真后
	 *  - 其他：单 phase = (0, 0, "")
	 *
	 * 改动这套规则必须 bump 两个 indexer 的 IndexerVersion，否则 DDC 旧 artifact 还会以同 identity 命中。
	 */
	inline TArray<FAssetVisualPhaseDef> GetPhasesForAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return { FAssetVisualPhaseDef{ 0, 0.0f, FString() } };
		}

		if (UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Asset))
		{
			if (AnimHasPreviewMesh(Anim))
			{
				return {
					FAssetVisualPhaseDef{ 0, 0.25f, TEXT("early") },
					FAssetVisualPhaseDef{ 1, 0.50f, TEXT("middle") },
					FAssetVisualPhaseDef{ 2, 0.75f, TEXT("late") },
				};
			}
			// 没 preview mesh，多 phase 渲不出来；退化为单 phase 让 thumbnail manager 兜底渲一帧。
			return { FAssetVisualPhaseDef{ 0, 0.0f, FString() } };
		}
		if (Cast<UNiagaraSystem>(Asset))
		{
			return {
				FAssetVisualPhaseDef{ 0, 0.5f, TEXT("burst") },
				FAssetVisualPhaseDef{ 1, 1.5f, TEXT("peak") },
				FAssetVisualPhaseDef{ 2, 3.0f, TEXT("tail") },
			};
		}
		// 默认单 phase：StaticMesh / SkeletalMesh / Material / Widget / NiagaraEmitter / AnimBlueprint。
		// AnimBlueprint 多 phase 需要实例化 AnimInstance 驱动 graph，复杂度大；先单 phase 走 thumbnail
		// manager 兜底。
		return { FAssetVisualPhaseDef{ 0, 0.0f, FString() } };
	}

	/** 把资产类映射成 renderer 的 AssetClassHint（FName）；renderer 用它选 anim / niagara 旁路。
	 *  返回 NAME_None 表示让 renderer 自己 Cast 探测（默认 thumbnail manager 路径）。 */
	inline FName GetRendererAssetClassHint(UObject* Asset)
	{
		if (!Asset)
		{
			return NAME_None;
		}
		if (Cast<UAnimMontage>(Asset))
		{
			return FName(TEXT("AnimMontage"));
		}
		if (Cast<UAnimSequence>(Asset))
		{
			return FName(TEXT("AnimSequence"));
		}
		if (Cast<UAnimSequenceBase>(Asset))
		{
			// AnimComposite 等其他 SequenceBase 子类，仍走 anim 旁路。
			return FName(TEXT("AnimSequence"));
		}
		if (Cast<UNiagaraSystem>(Asset))
		{
			return FName(TEXT("NiagaraSystem"));
		}
		return NAME_None;
	}
}
