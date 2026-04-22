#include "Indexers/AnimationIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * 这份实现文件把多种动画资产统一压成“单 node 摘要”。
 *
 * 这么做的原因是：
 * - 查询侧通常更关心动画的关键元数据，而不是逐帧内容；
 * - 单 node 载荷更适合 artifact cache 和 shadow diff；
 * - 不同动画类型虽然长得不一样，但都能收口成一个 properties JSON。
 */

bool FAnimationIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!BuildPayload(LoadedAsset, Payload.Node))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeNodePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FAnimationIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayload(Payload, DB, AssetId);
}

bool FAnimationIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FAnimationIndexer::BuildPayload(UObject* LoadedAsset, FIndexedNode& OutNode) const
{
	// 这里按“更具体的类型优先”往下分发，
	// 因为像 Montage/BlendSpace 都可能同时也是更宽泛动画类体系的一员。
	if (UAnimMontage* Montage = Cast<UAnimMontage>(LoadedAsset))
	{
		return BuildAnimMontageNode(Montage, OutNode);
	}

	if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(LoadedAsset))
	{
		return BuildBlendSpaceNode(BlendSpace, OutNode);
	}

	if (UPoseAsset* PoseAsset = Cast<UPoseAsset>(LoadedAsset))
	{
		return BuildPoseAssetNode(PoseAsset, OutNode);
	}

	if (UAnimSequence* AnimSeq = Cast<UAnimSequence>(LoadedAsset))
	{
		return BuildAnimSequenceNode(AnimSeq, OutNode);
	}

	return false;
}

bool FAnimationIndexer::BuildAnimSequenceNode(UAnimSequence* AnimSeq, FIndexedNode& OutNode) const
{
	if (!AnimSeq)
	{
		return false;
	}

	USkeleton* Skeleton = AnimSeq->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);
	Props->SetNumberField(TEXT("length"), AnimSeq->GetPlayLength());
	Props->SetNumberField(TEXT("num_frames"), AnimSeq->GetNumberOfSampledKeys());
	Props->SetNumberField(TEXT("rate_scale"), AnimSeq->RateScale);

	TArray<TSharedPtr<FJsonValue>> TracksArr;
	if (Skeleton)
	{
		// 这里记录的是骨骼名字列表，不是每条轨道的完整关键帧数据。
		// 目的是给搜索和结构摘要用，而不是做动画重建。
		const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
		const int32 NumBones = RefSkeleton.GetNum();
		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			TracksArr.Add(MakeShared<FJsonValueString>(RefSkeleton.GetBoneName(BoneIdx).ToString()));
		}
	}
	Props->SetArrayField(TEXT("bone_tracks"), TracksArr);

	TArray<TSharedPtr<FJsonValue>> CurvesArr;
	const FRawCurveTracks& RawCurves = AnimSeq->GetCurveData();
	for (const FFloatCurve& Curve : RawCurves.FloatCurves)
	{
		// 曲线只保留“名字 + key 数量”这类轻量摘要，避免 payload 过大。
		auto CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetNumberField(TEXT("num_keys"), Curve.FloatCurve.GetNumKeys());
		CurvesArr.Add(MakeShared<FJsonValueObject>(CurveObj));
	}
	Props->SetArrayField(TEXT("curves"), CurvesArr);
	Props->SetStringField(TEXT("notifies"), NotifiesToJson(AnimSeq->Notifies));

	OutNode = FIndexedNode();
	OutNode.NodeType = TEXT("AnimSequence");
	OutNode.NodeName = AnimSeq->GetName();
	OutNode.NodeClass = TEXT("UAnimSequence");

	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutNode.Properties);
	return FJsonSerializer::Serialize(Props, *Writer, true);
}

bool FAnimationIndexer::BuildAnimMontageNode(UAnimMontage* Montage, FIndexedNode& OutNode) const
{
	if (!Montage)
	{
		return false;
	}

	USkeleton* Skeleton = Montage->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);
	Props->SetNumberField(TEXT("length"), Montage->GetPlayLength());

	TArray<TSharedPtr<FJsonValue>> SectionsArr;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		// Montage 的 section 信息对“跳转逻辑”很关键，所以这里会单独记下来。
		auto SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
		SectionObj->SetNumberField(TEXT("start_time"), Section.GetTime());
		SectionObj->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
		SectionsArr.Add(MakeShared<FJsonValueObject>(SectionObj));
	}
	Props->SetArrayField(TEXT("sections"), SectionsArr);

	TArray<TSharedPtr<FJsonValue>> SlotsArr;
	for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
	{
		// slot 里不展开全部 segment，只先记录 slot 名和段数。
		auto SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetStringField(TEXT("name"), Slot.SlotName.ToString());
		SlotObj->SetNumberField(TEXT("num_segments"), Slot.AnimTrack.AnimSegments.Num());
		SlotsArr.Add(MakeShared<FJsonValueObject>(SlotObj));
	}
	Props->SetArrayField(TEXT("slots"), SlotsArr);
	Props->SetStringField(TEXT("notifies"), NotifiesToJson(Montage->Notifies));

	OutNode = FIndexedNode();
	OutNode.NodeType = TEXT("AnimMontage");
	OutNode.NodeName = Montage->GetName();
	OutNode.NodeClass = TEXT("UAnimMontage");

	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutNode.Properties);
	return FJsonSerializer::Serialize(Props, *Writer, true);
}

bool FAnimationIndexer::BuildBlendSpaceNode(UBlendSpace* BlendSpace, FIndexedNode& OutNode) const
{
	if (!BlendSpace)
	{
		return false;
	}

	USkeleton* Skeleton = BlendSpace->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);

	const FBlendParameter& AxisX = BlendSpace->GetBlendParameter(0);
	const FBlendParameter& AxisY = BlendSpace->GetBlendParameter(1);

	auto AxisXObj = MakeShared<FJsonObject>();
	AxisXObj->SetStringField(TEXT("name"), AxisX.DisplayName);
	AxisXObj->SetNumberField(TEXT("min"), AxisX.Min);
	AxisXObj->SetNumberField(TEXT("max"), AxisX.Max);
	AxisXObj->SetNumberField(TEXT("grid_num"), AxisX.GridNum);
	Props->SetObjectField(TEXT("axis_x"), AxisXObj);

	auto AxisYObj = MakeShared<FJsonObject>();
	AxisYObj->SetStringField(TEXT("name"), AxisY.DisplayName);
	AxisYObj->SetNumberField(TEXT("min"), AxisY.Min);
	AxisYObj->SetNumberField(TEXT("max"), AxisY.Max);
	AxisYObj->SetNumberField(TEXT("grid_num"), AxisY.GridNum);
	Props->SetObjectField(TEXT("axis_y"), AxisYObj);

	TArray<TSharedPtr<FJsonValue>> SamplesArr;
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
	for (const FBlendSample& Sample : Samples)
	{
		// BlendSpace 的采样点是理解动画混合布局最重要的摘要之一。
		auto SampleObj = MakeShared<FJsonObject>();
		SampleObj->SetStringField(TEXT("animation"), Sample.Animation ? Sample.Animation->GetPathName() : TEXT("None"));
		SampleObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
		SampleObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
		SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
		SamplesArr.Add(MakeShared<FJsonValueObject>(SampleObj));
	}
	Props->SetArrayField(TEXT("sample_points"), SamplesArr);

	OutNode = FIndexedNode();
	OutNode.NodeType = TEXT("BlendSpace");
	OutNode.NodeName = BlendSpace->GetName();
	OutNode.NodeClass = TEXT("UBlendSpace");

	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutNode.Properties);
	return FJsonSerializer::Serialize(Props, *Writer, true);
}

bool FAnimationIndexer::BuildPoseAssetNode(UPoseAsset* PoseAsset, FIndexedNode& OutNode) const
{
	if (!PoseAsset)
	{
		return false;
	}

	auto Props = MakeShared<FJsonObject>();
	if (PoseAsset->GetSkeleton())
	{
		Props->SetStringField(TEXT("skeleton"), PoseAsset->GetSkeleton()->GetPathName());
	}

	Props->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
	Props->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
	Props->SetNumberField(TEXT("num_curves"), PoseAsset->GetNumCurves());
	Props->SetBoolField(TEXT("is_additive"), PoseAsset->IsValidAdditive());

	if (!PoseAsset->RetargetSource.IsNone())
	{
		Props->SetStringField(TEXT("retarget_source"), PoseAsset->RetargetSource.ToString());
	}

	const TArray<FName>& PoseNames = PoseAsset->GetPoseFNames();
	TArray<TSharedPtr<FJsonValue>> PoseNameArray;
	for (const FName& Name : PoseNames)
	{
		PoseNameArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	Props->SetArrayField(TEXT("pose_names"), PoseNameArray);

	OutNode = FIndexedNode();
	OutNode.NodeType = TEXT("PoseAsset");
	OutNode.NodeName = PoseAsset->GetName();
	OutNode.NodeClass = TEXT("PoseAsset");

	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutNode.Properties);
	return FJsonSerializer::Serialize(Props, *Writer, true);
}

FString FAnimationIndexer::NotifiesToJson(const TArray<FAnimNotifyEvent>& Notifies)
{
	TArray<TSharedPtr<FJsonValue>> NotifyArr;
	for (const FAnimNotifyEvent& Notify : Notifies)
	{
		auto NotifyObj = MakeShared<FJsonObject>();
		NotifyObj->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		NotifyObj->SetNumberField(TEXT("trigger_time"), Notify.GetTriggerTime());
		NotifyObj->SetNumberField(TEXT("duration"), Notify.GetDuration());

		if (Notify.Notify)
		{
			NotifyObj->SetStringField(TEXT("class"), Notify.Notify->GetClass()->GetName());
		}
		else if (Notify.NotifyStateClass)
		{
			NotifyObj->SetStringField(TEXT("class"), Notify.NotifyStateClass->GetClass()->GetName());
			NotifyObj->SetBoolField(TEXT("is_state"), true);
		}

		NotifyArr.Add(MakeShared<FJsonValueObject>(NotifyObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(NotifyArr, *Writer);
	return Result;
}
