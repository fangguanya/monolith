#pragma once

#include "MonolithIndexer.h"

/*
 * FAnimationIndexer 负责把动画资产压缩成可搜索、可缓存的一条 node。
 *
 * 动画不像 Blueprint 那样要记录大图结构，
 * 所以这里的重点不是“连线”，而是把最有用的摘要提出来：
 * - Skeleton
 * - 时长、帧数、曲线
 * - Montage section / slot
 * - BlendSpace 采样点
 * - PoseAsset 的 pose 数量
 */
class FAnimationIndexer : public IMonolithIndexer
{
public:
	/** 支持的一组动画类。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {
			TEXT("AnimSequence"),
			TEXT("AnimMontage"),
			TEXT("BlendSpace"),
			TEXT("BlendSpace1D"),
			TEXT("AimOffsetBlendSpace"),
			TEXT("AimOffsetBlendSpace1D"),
			TEXT("PoseAsset")
		};
	}

	/** 稳定 cohort 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Animation")); }
	/** 日志展示名。 */
	virtual FString GetName() const override { return TEXT("AnimationIndexer"); }
	/** 构建 artifact。 */
	virtual bool BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact) override;
	/** 回放 artifact 到正式表。 */
	virtual bool MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId) override;
	/** 回放 artifact 到 shadow 表。 */
	virtual bool MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) override;

private:
	/** 根据具体动画对象构造统一 node。 */
	bool BuildPayload(UObject* LoadedAsset, FIndexedNode& OutNode) const;
	/** 读取 AnimSequence 专属字段。 */
	bool BuildAnimSequenceNode(class UAnimSequence* AnimSeq, FIndexedNode& OutNode) const;
	/** 读取 Montage 专属字段。 */
	bool BuildAnimMontageNode(class UAnimMontage* Montage, FIndexedNode& OutNode) const;
	/** 读取 BlendSpace 系列字段。 */
	bool BuildBlendSpaceNode(class UBlendSpace* BlendSpace, FIndexedNode& OutNode) const;
	/** 读取 PoseAsset 字段。 */
	bool BuildPoseAssetNode(class UPoseAsset* PoseAsset, FIndexedNode& OutNode) const;
	/** 把 notify 列表变成 JSON 文本。 */
	static FString NotifiesToJson(const TArray<struct FAnimNotifyEvent>& Notifies);
};
