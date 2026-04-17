// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithZoneGraphActions.h"

#if WITH_ZONEGRAPH
#include "ZoneGraphData.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneGraphTypes.h"
#include "ZoneGraphSettings.h"
#include "ZoneShapeActor.h"
#include "ZoneShapeComponent.h"
#include "ZoneGraphBuilder.h"
#include "ZoneGraphAStar.h"
#include "ZoneGraphQuery.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#endif // WITH_ZONEGRAPH

void FMonolithZoneGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_ZONEGRAPH
	Registry.RegisterAction(TEXT("zg"), TEXT("list_zone_graph_data"),
		TEXT("List AZoneGraphData actors in the current world."),
		FMonolithActionHandler::CreateStatic(&HandleListZoneGraphData),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("list_zone_shapes"),
		TEXT("List AZoneShape actors in the current world."),
		FMonolithActionHandler::CreateStatic(&HandleListZoneShapes),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_tag"), TEXT("string"), TEXT("Only include actors with this tag"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("get_zone_shape_config"),
		TEXT("Inspect a single AZoneShape: points, tags, polygon routing type, lane profile."),
		FMonolithActionHandler::CreateStatic(&HandleGetZoneShapeConfig),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("AZoneShape actor label"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("list_lane_tags"),
		TEXT("Enumerate the fixed list of ZoneGraph tags declared in Project Settings."),
		FMonolithActionHandler::CreateStatic(&HandleListLaneTags),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("build_zone_graph"),
		TEXT("Invoke FZoneGraphBuilder::BuildAll on all AZoneGraphData actors in the world."),
		FMonolithActionHandler::CreateStatic(&HandleBuildZoneGraph),
		FParamSchemaBuilder()
			.Optional(TEXT("force_rebuild"), TEXT("boolean"), TEXT("Force full rebuild (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("query_lanes_by_tag"),
		TEXT("Return indices of lanes whose tag mask includes the named tag."),
		FMonolithActionHandler::CreateStatic(&HandleQueryLanesByTag),
		FParamSchemaBuilder()
			.Required(TEXT("tag_name"), TEXT("string"), TEXT("Tag name declared in ZoneGraphSettings"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("get_storage_summary"),
		TEXT("Return per-AZoneGraphData counts: zones, lanes, points, bounds."),
		FMonolithActionHandler::CreateStatic(&HandleGetStorageSummary),
		FParamSchemaBuilder().Build());

	// CL-B.7 self-test tools — see class-level header comment for rationale + expected callers.

	Registry.RegisterAction(TEXT("zg"), TEXT("get_lane_info"),
		TEXT("Geometry + tag mask for the lane at (data_index, lane_index). Replaces the "
		     "bugged ai.get_zone_lane_info which ignored the handle."),
		FMonolithActionHandler::CreateStatic(&HandleGetLaneInfo),
		FParamSchemaBuilder()
			.Required(TEXT("data_index"), TEXT("integer"), TEXT("AZoneGraphData index in iteration order"))
			.Required(TEXT("lane_index"), TEXT("integer"), TEXT("Lane index within FZoneGraphStorage::Lanes"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("get_lane_links"),
		TEXT("NextLanes / PrevLanes / AdjacentLanes for a lane, with EZoneLaneLinkType + EZoneLaneLinkFlags."),
		FMonolithActionHandler::CreateStatic(&HandleGetLaneLinks),
		FParamSchemaBuilder()
			.Required(TEXT("data_index"), TEXT("integer"), TEXT("AZoneGraphData index"))
			.Required(TEXT("lane_index"), TEXT("integer"), TEXT("Lane index within storage.Lanes"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("detect_lane_islands"),
		TEXT("Flood-fill on lane link connectivity; returns per-island lane-count report. "
		     "island_count == 1 ⇒ fully-connected bake; higher ⇒ broken junction."),
		FMonolithActionHandler::CreateStatic(&HandleDetectLaneIslands),
		FParamSchemaBuilder()
			.Optional(TEXT("tag_filter_any"), TEXT("string"),
				TEXT("Only consider lanes whose tags include any of these (comma-separated). Empty=all lanes."))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("trace_lane_path"),
		TEXT("FZoneGraphAStar::FindPath from start to end lane with optional tag filters. "
		     "Returns lane-handle sequence. Used by TC-9 to validate turn restrictions."),
		FMonolithActionHandler::CreateStatic(&HandleTraceLanePath),
		FParamSchemaBuilder()
			.Required(TEXT("start_data_index"), TEXT("integer"), TEXT("Start lane's AZoneGraphData index"))
			.Required(TEXT("start_lane_index"), TEXT("integer"), TEXT("Start lane index"))
			.Required(TEXT("end_data_index"), TEXT("integer"), TEXT("End lane's AZoneGraphData index"))
			.Required(TEXT("end_lane_index"), TEXT("integer"), TEXT("End lane index"))
			.Optional(TEXT("tag_filter_any"), TEXT("string"), TEXT("Require lane tags include any of these (comma-separated)"))
			.Optional(TEXT("tag_filter_not"), TEXT("string"), TEXT("Exclude lanes whose tags include any of these"))
			.Build());

	Registry.RegisterAction(TEXT("zg"), TEXT("list_polygon_shape_points"),
		TEXT("Dump Shape Points of a Polygon AZoneShape: Position, Type, per-point LaneProfile index."),
		FMonolithActionHandler::CreateStatic(&HandleListPolygonShapePoints),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("AZoneShape actor label; must be Polygon type"))
			.Build());
#else
	(void)Registry;
	UE_LOG(LogMonolithMass, Warning, TEXT("zg.* namespace not registered: WITH_ZONEGRAPH=0"));
#endif
}

#if WITH_ZONEGRAPH

namespace
{
	/** Resolve a ZoneGraph tag by its authored name. */
	bool ResolveTagByName(FName TagName, FZoneGraphTag& OutTag, FString& OutError)
	{
		const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
		if (!Settings)
		{
			OutError = TEXT("UZoneGraphSettings unavailable");
			return false;
		}
		const TConstArrayView<FZoneGraphTagInfo> TagInfos = Settings->GetTagInfos();
		for (const FZoneGraphTagInfo& Info : TagInfos)
		{
			if (Info.Name == TagName)
			{
				OutTag = Info.Tag;
				return OutTag.IsValid();
			}
		}
		OutError = FString::Printf(TEXT("Tag '%s' not declared"), *TagName.ToString());
		return false;
	}
}

FMonolithActionResult FMonolithZoneGraphActions::HandleListZoneGraphData(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		Entry->SetStringField(TEXT("actor_name"), It->GetName());
		Entry->SetObjectField(TEXT("location"), MonolithMass::VectorToJson(It->GetActorLocation()));
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("zone_graph_data"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleListZoneShapes(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	FName TagFilter = NAME_None;
	if (Params.IsValid())
	{
		const FString TagStr = Params->GetStringField(TEXT("actor_tag"));
		if (!TagStr.IsEmpty()) { TagFilter = FName(*TagStr); }
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TActorIterator<AZoneShape> It(World); It; ++It)
	{
		if (!TagFilter.IsNone() && !It->Tags.Contains(TagFilter)) { continue; }
		const UZoneShapeComponent* Comp = It->GetShape();

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		Entry->SetStringField(TEXT("actor_name"), It->GetName());
		Entry->SetNumberField(TEXT("point_count"), Comp ? Comp->GetPoints().Num() : 0);
		Entry->SetObjectField(TEXT("location"), MonolithMass::VectorToJson(It->GetActorLocation()));
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("zone_shapes"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleGetZoneShapeConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	const FName Label = MonolithMass::ReadRequiredName(Params, TEXT("actor_label"), Err);
	if (Label.IsNone()) { return FMonolithActionResult::Error(Err); }

	for (TActorIterator<AZoneShape> It(World); It; ++It)
	{
		if (FName(*It->GetActorLabel()) != Label) { continue; }

		const UZoneShapeComponent* Comp = It->GetShape();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		if (!Comp)
		{
			Result->SetStringField(TEXT("warning"), TEXT("Actor has no UZoneShapeComponent"));
			return FMonolithActionResult::Success(Result);
		}

		Result->SetStringField(TEXT("shape_type"),
			Comp->GetShapeType() == FZoneShapeType::Polygon ? TEXT("Polygon") : TEXT("Spline"));

		TArray<TSharedPtr<FJsonValue>> PointsArr;
		for (const FZoneShapePoint& P : Comp->GetPoints())
		{
			TSharedPtr<FJsonObject> PJ = MakeShared<FJsonObject>();
			PJ->SetObjectField(TEXT("position"), MonolithMass::VectorToJson(P.Position));
			PJ->SetNumberField(TEXT("tangent_length"), P.TangentLength);
			PointsArr.Add(MakeShared<FJsonValueObject>(PJ));
		}
		Result->SetArrayField(TEXT("points"), PointsArr);
		Result->SetNumberField(TEXT("tag_mask_bits"), Comp->GetTags().GetValue());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("AZoneShape with label '%s' not found"), *Label.ToString()));
}

FMonolithActionResult FMonolithZoneGraphActions::HandleListLaneTags(const TSharedPtr<FJsonObject>& Params)
{
	const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UZoneGraphSettings unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	const TConstArrayView<FZoneGraphTagInfo> TagInfos = Settings->GetTagInfos();
	for (const FZoneGraphTagInfo& Info : TagInfos)
	{
		if (Info.Name.IsNone()) { continue; }
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		Entry->SetNumberField(TEXT("bit_index"), Info.Tag.Get());
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tags"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleBuildZoneGraph(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	UZoneGraphSubsystem* ZGS = UWorld::GetSubsystem<UZoneGraphSubsystem>(World);
	if (!ZGS)
	{
		return FMonolithActionResult::Error(TEXT("UZoneGraphSubsystem unavailable"));
	}

	bool bForce = true;
	if (Params.IsValid() && Params->HasField(TEXT("force_rebuild")))
	{
		bForce = Params->GetBoolField(TEXT("force_rebuild"));
	}

	TArray<AZoneGraphData*> DataActors;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It)
	{
		DataActors.Add(*It);
	}
	if (DataActors.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No AZoneGraphData actors in world"));
	}

	ZGS->GetBuilder().BuildAll(DataActors, bForce);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("data_actor_count"), DataActors.Num());
	Result->SetBoolField(TEXT("force_rebuild"), bForce);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("build_zone_graph requires an editor build"));
#endif
}

FMonolithActionResult FMonolithZoneGraphActions::HandleQueryLanesByTag(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	const FName TagName = MonolithMass::ReadRequiredName(Params, TEXT("tag_name"), Err);
	if (TagName.IsNone()) { return FMonolithActionResult::Error(Err); }

	FZoneGraphTag Tag;
	if (!ResolveTagByName(TagName, Tag, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	int32 MatchingLanes = 0;
	int32 TotalLanes = 0;
	TArray<TSharedPtr<FJsonValue>> PerDataArr;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It)
	{
		const FZoneGraphStorage& Storage = It->GetStorage();
		int32 MatchesHere = 0;
		for (const FZoneLaneData& Lane : Storage.Lanes)
		{
			++TotalLanes;
			if (Lane.Tags.Contains(Tag)) { ++MatchesHere; }
		}
		MatchingLanes += MatchesHere;

		TSharedPtr<FJsonObject> PD = MakeShared<FJsonObject>();
		PD->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		PD->SetNumberField(TEXT("matching_lanes"), MatchesHere);
		PerDataArr.Add(MakeShared<FJsonValueObject>(PD));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag_name"), TagName.ToString());
	Result->SetNumberField(TEXT("tag_bit_index"), Tag.Get());
	Result->SetNumberField(TEXT("matching_lanes"), MatchingLanes);
	Result->SetNumberField(TEXT("total_lanes"), TotalLanes);
	Result->SetArrayField(TEXT("per_zone_graph_data"), PerDataArr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleGetStorageSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It)
	{
		const FZoneGraphStorage& Storage = It->GetStorage();
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		Entry->SetNumberField(TEXT("zones"), Storage.Zones.Num());
		Entry->SetNumberField(TEXT("lanes"), Storage.Lanes.Num());
		Entry->SetNumberField(TEXT("lane_points"), Storage.LanePoints.Num());
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("summaries"), Arr);
	return FMonolithActionResult::Success(Result);
}

// =============================================================================================
// CL-B.7 self-test handlers
// =============================================================================================

namespace
{
	/**
	 * Shared helper: resolve (data_index, lane_index) pair to an FZoneGraphStorage + validated lane index.
	 * Returns false + populates OutError if either index is out of range.
	 */
	bool ResolveLaneHandle(UWorld* World, int32 DataIndex, int32 LaneIndex,
		const FZoneGraphStorage*& OutStorage, FString& OutError)
	{
		OutStorage = nullptr;
		int32 SeenDataIndex = 0;
		for (TActorIterator<AZoneGraphData> It(World); It; ++It)
		{
			if (SeenDataIndex == DataIndex)
			{
				const FZoneGraphStorage& Storage = It->GetStorage();
				if (LaneIndex < 0 || LaneIndex >= Storage.Lanes.Num())
				{
					OutError = FString::Printf(
						TEXT("lane_index %d out of range [0, %d) for AZoneGraphData[%d]"),
						LaneIndex, Storage.Lanes.Num(), DataIndex);
					return false;
				}
				OutStorage = &Storage;
				return true;
			}
			++SeenDataIndex;
		}
		OutError = FString::Printf(TEXT("data_index %d out of range (found %d AZoneGraphData actor(s))"),
			DataIndex, SeenDataIndex);
		return false;
	}

	/** Parse a comma-separated tag name list into a tag mask. Unknown tag names are silently dropped. */
	FZoneGraphTagMask ParseTagMaskFromCsv(const FString& Csv)
	{
		FZoneGraphTagMask Mask;
		if (Csv.IsEmpty()) { return Mask; }

		const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
		if (!Settings) { return Mask; }

		TArray<FString> Parts;
		Csv.ParseIntoArray(Parts, TEXT(","), /*CullEmpty*/ true);

		const TConstArrayView<FZoneGraphTagInfo> TagInfos = Settings->GetTagInfos();
		for (const FString& Part : Parts)
		{
			const FName N(*Part.TrimStartAndEnd());
			for (const FZoneGraphTagInfo& Info : TagInfos)
			{
				if (Info.Name == N)
				{
					Mask.Add(Info.Tag);
					break;
				}
			}
		}
		return Mask;
	}

	FString LinkTypeToString(EZoneLaneLinkType Type)
	{
		switch (Type)
		{
		case EZoneLaneLinkType::Outgoing: return TEXT("Outgoing");
		case EZoneLaneLinkType::Incoming: return TEXT("Incoming");
		case EZoneLaneLinkType::Adjacent: return TEXT("Adjacent");
		case EZoneLaneLinkType::All:      return TEXT("All");
		default:                          return TEXT("None");
		}
	}

	/** Build a compact JSON representation of a lane-link destination handle + classification. */
	TSharedPtr<FJsonObject> LinkToJson(const FZoneLaneLinkData& Link, int32 DataIndex)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("dest_lane_index"), Link.DestLaneIndex);
		Obj->SetNumberField(TEXT("data_index"), DataIndex);
		Obj->SetStringField(TEXT("type"), LinkTypeToString(Link.Type));
		Obj->SetNumberField(TEXT("flags_bits"), int32(Link.Flags));
		return Obj;
	}
}

FMonolithActionResult FMonolithZoneGraphActions::HandleGetLaneInfo(const TSharedPtr<FJsonObject>& Params)
{
	// Replaces the bugged ai.get_zone_lane_info which ignored the handle and always returned
	// lane 0. Callers address the lane explicitly via (data_index, lane_index); we resolve via
	// iteration so callers don't need to know AZoneGraphData's internal uint16 generation counter.
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	int32 DataIndex = 0, LaneIndex = 0;
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("data_index"), DataIndex, Err)) { return FMonolithActionResult::Error(Err); }
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("lane_index"), LaneIndex, Err)) { return FMonolithActionResult::Error(Err); }

	const FZoneGraphStorage* Storage = nullptr;
	if (!ResolveLaneHandle(World, DataIndex, LaneIndex, Storage, Err)) { return FMonolithActionResult::Error(Err); }

	const FZoneLaneData& Lane = Storage->Lanes[LaneIndex];

	// Compute physical lane length by summing segment lengths — cheap given typical lane
	// has <30 points. Inline keeps the response shape obvious; avoids the indirection of
	// UE::ZoneGraph::Query::GetLaneLength.
	// NB: use `Pi` (not `PI`) — UE's header suite defines a `PI` macro (math constant).
	float LaneLength = 0.0f;
	for (int32 Pi = Lane.PointsBegin + 1; Pi < Lane.PointsEnd; ++Pi)
	{
		LaneLength += static_cast<float>((Storage->LanePoints[Pi] - Storage->LanePoints[Pi - 1]).Size());
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("data_index"), DataIndex);
	Result->SetNumberField(TEXT("lane_index"), LaneIndex);
	Result->SetNumberField(TEXT("width"), Lane.Width);
	Result->SetNumberField(TEXT("zone_index"), Lane.ZoneIndex);
	Result->SetNumberField(TEXT("num_points"), Lane.GetNumPoints());
	Result->SetNumberField(TEXT("num_links"), Lane.GetLinkCount());
	Result->SetNumberField(TEXT("tags_bits"), Lane.Tags.GetValue());
	Result->SetNumberField(TEXT("length"), LaneLength);
	if (Lane.GetNumPoints() > 0)
	{
		Result->SetObjectField(TEXT("start_point"), MonolithMass::VectorToJson(Storage->LanePoints[Lane.PointsBegin]));
		Result->SetObjectField(TEXT("end_point"),   MonolithMass::VectorToJson(Storage->LanePoints[Lane.PointsEnd - 1]));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleGetLaneLinks(const TSharedPtr<FJsonObject>& Params)
{
	// Enumerate every FZoneLaneLinkData in [LinksBegin, LinksEnd) and partition into
	// Outgoing / Incoming / Adjacent buckets. Consumer logic (TC-3/4/5) expects exact
	// classification to distinguish lane-to-lane forward flow vs side-by-side adjacency.
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	int32 DataIndex = 0, LaneIndex = 0;
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("data_index"), DataIndex, Err)) { return FMonolithActionResult::Error(Err); }
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("lane_index"), LaneIndex, Err)) { return FMonolithActionResult::Error(Err); }

	const FZoneGraphStorage* Storage = nullptr;
	if (!ResolveLaneHandle(World, DataIndex, LaneIndex, Storage, Err)) { return FMonolithActionResult::Error(Err); }

	const FZoneLaneData& Lane = Storage->Lanes[LaneIndex];

	TArray<TSharedPtr<FJsonValue>> NextArr, PrevArr, AdjArr;
	for (int32 LI = Lane.LinksBegin; LI < Lane.LinksEnd; ++LI)
	{
		const FZoneLaneLinkData& Link = Storage->LaneLinks[LI];
		TSharedPtr<FJsonObject> Obj = LinkToJson(Link, DataIndex);
		switch (Link.Type)
		{
		case EZoneLaneLinkType::Outgoing: NextArr.Add(MakeShared<FJsonValueObject>(Obj)); break;
		case EZoneLaneLinkType::Incoming: PrevArr.Add(MakeShared<FJsonValueObject>(Obj)); break;
		case EZoneLaneLinkType::Adjacent: AdjArr.Add(MakeShared<FJsonValueObject>(Obj));  break;
		default: break;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("data_index"), DataIndex);
	Result->SetNumberField(TEXT("lane_index"), LaneIndex);
	Result->SetArrayField(TEXT("next_lanes"), NextArr);
	Result->SetArrayField(TEXT("prev_lanes"), PrevArr);
	Result->SetArrayField(TEXT("adjacent_lanes"), AdjArr);
	Result->SetNumberField(TEXT("total_links"), Lane.GetLinkCount());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleDetectLaneIslands(const TSharedPtr<FJsonObject>& Params)
{
	// Flood-fill every lane via Outgoing + Incoming + Adjacent edges treated as undirected —
	// a "reachable" set for connectivity, not for directional routing. Adjacent links included
	// so same-zone lanes (e.g. 2 Vehicle lanes side by side) count as one island.
	//
	// Complexity O(N + E) where N = total lanes, E = total link entries. Safe for 10^5 lanes.
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	FZoneGraphTagMask FilterAnyMask;
	if (Params.IsValid())
	{
		const FString Csv = Params->GetStringField(TEXT("tag_filter_any"));
		FilterAnyMask = ParseTagMaskFromCsv(Csv);
	}
	const bool bUseFilter = FilterAnyMask != FZoneGraphTagMask();

	// Collect all lanes across all data actors into a single flat array so a single
	// union-find can span multi-data worlds. Key = (data_index << 32) | lane_index.
	struct FLaneKey { int32 Data; int32 Lane; };
	TArray<FLaneKey> AllLanes;
	TArray<const FZoneGraphStorage*> Storages;
	int32 DataIdx = 0;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It)
	{
		const FZoneGraphStorage& Storage = It->GetStorage();
		Storages.Add(&Storage);
		for (int32 L = 0; L < Storage.Lanes.Num(); ++L)
		{
			if (bUseFilter && !Storage.Lanes[L].Tags.ContainsAny(FilterAnyMask)) { continue; }
			AllLanes.Add({DataIdx, L});
		}
		++DataIdx;
	}
	const int32 N = AllLanes.Num();

	// Map (data, lane) → flat index for the union-find universe.
	TMap<uint64, int32> LaneToFlat;
	LaneToFlat.Reserve(N);
	for (int32 i = 0; i < N; ++i)
	{
		const uint64 Key = (uint64(AllLanes[i].Data) << 32) | uint32(AllLanes[i].Lane);
		LaneToFlat.Add(Key, i);
	}

	// Union-find with path compression + union-by-rank.
	TArray<int32> Parent, Rank;
	Parent.SetNum(N); Rank.SetNumZeroed(N);
	for (int32 i = 0; i < N; ++i) { Parent[i] = i; }
	auto Find = [&](int32 x) { while (Parent[x] != x) { Parent[x] = Parent[Parent[x]]; x = Parent[x]; } return x; };
	auto Union = [&](int32 a, int32 b)
	{
		a = Find(a); b = Find(b);
		if (a == b) { return; }
		if (Rank[a] < Rank[b])      { Parent[a] = b; }
		else if (Rank[a] > Rank[b]) { Parent[b] = a; }
		else                        { Parent[b] = a; ++Rank[a]; }
	};

	for (int32 i = 0; i < N; ++i)
	{
		const FLaneKey& LK = AllLanes[i];
		const FZoneGraphStorage& Storage = *Storages[LK.Data];
		const FZoneLaneData& Lane = Storage.Lanes[LK.Lane];
		for (int32 LI = Lane.LinksBegin; LI < Lane.LinksEnd; ++LI)
		{
			const FZoneLaneLinkData& Link = Storage.LaneLinks[LI];
			// Link destination is always in the same AZoneGraphData (ZG Builder doesn't
			// currently stitch across data actors). Cross-data stitching would require
			// additional proximity connectors which we don't emit.
			const uint64 DestKey = (uint64(LK.Data) << 32) | uint32(Link.DestLaneIndex);
			if (const int32* DestFlat = LaneToFlat.Find(DestKey))
			{
				Union(i, *DestFlat);
			}
		}
	}

	// Build per-island summary: count lanes + pick one sample handle + track largest size.
	TMap<int32, int32> RootToCount;
	TMap<int32, FLaneKey> RootToSample;
	for (int32 i = 0; i < N; ++i)
	{
		const int32 R = Find(i);
		RootToCount.FindOrAdd(R) += 1;
		if (!RootToSample.Contains(R)) { RootToSample.Add(R, AllLanes[i]); }
	}

	TArray<TSharedPtr<FJsonValue>> IslandArr;
	int32 LargestSize = 0;
	for (const auto& Pair : RootToCount)
	{
		const FLaneKey& Sample = RootToSample[Pair.Key];
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("lane_count"), Pair.Value);
		Obj->SetNumberField(TEXT("sample_data_index"), Sample.Data);
		Obj->SetNumberField(TEXT("sample_lane_index"), Sample.Lane);
		IslandArr.Add(MakeShared<FJsonValueObject>(Obj));
		LargestSize = FMath::Max(LargestSize, Pair.Value);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_lanes_considered"), N);
	Result->SetNumberField(TEXT("island_count"), IslandArr.Num());
	Result->SetNumberField(TEXT("largest_island_lanes"), LargestSize);
	Result->SetArrayField(TEXT("islands"), IslandArr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleTraceLanePath(const TSharedPtr<FJsonObject>& Params)
{
	// FZoneGraphAStar::FindPath operates on a single FZoneGraphStorage; cross-data routing
	// isn't supported by the engine so we enforce start/end in the same AZoneGraphData.
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	int32 StartData = 0, StartLane = 0, EndData = 0, EndLane = 0;
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("start_data_index"), StartData, Err)) { return FMonolithActionResult::Error(Err); }
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("start_lane_index"), StartLane, Err)) { return FMonolithActionResult::Error(Err); }
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("end_data_index"),   EndData,   Err)) { return FMonolithActionResult::Error(Err); }
	if (!MonolithMass::ReadRequiredInt(Params, TEXT("end_lane_index"),   EndLane,   Err)) { return FMonolithActionResult::Error(Err); }

	if (StartData != EndData)
	{
		return FMonolithActionResult::Error(TEXT("FZoneGraphAStar requires start and end lanes "
			"to be in the same AZoneGraphData (cross-data routing is not implemented by the engine)."));
	}

	const FZoneGraphStorage* Storage = nullptr;
	if (!ResolveLaneHandle(World, StartData, StartLane, Storage, Err)) { return FMonolithActionResult::Error(Err); }
	const FZoneGraphStorage* StorageEnd = nullptr;
	if (!ResolveLaneHandle(World, EndData, EndLane, StorageEnd, Err)) { return FMonolithActionResult::Error(Err); }

	FZoneGraphTagFilter TagFilter;
	if (Params.IsValid())
	{
		TagFilter.AnyTags = ParseTagMaskFromCsv(Params->GetStringField(TEXT("tag_filter_any")));
		TagFilter.NotTags = ParseTagMaskFromCsv(Params->GetStringField(TEXT("tag_filter_not")));
	}

	// Pick the midpoint of each lane as the A* query position — produces well-defined
	// heuristic without caller needing to know lane arc length.
	auto MidpointOf = [Storage](int32 LaneIdx)
	{
		const FZoneLaneData& L = Storage->Lanes[LaneIdx];
		const int32 Mid = L.PointsBegin + (L.GetNumPoints() / 2);
		return Storage->LanePoints[FMath::Clamp(Mid, L.PointsBegin, L.PointsEnd - 1)];
	};

	FZoneGraphLaneLocation StartLoc, EndLoc;
	StartLoc.LaneHandle = FZoneGraphLaneHandle(StartLane, Storage->DataHandle);
	StartLoc.Position   = MidpointOf(StartLane);
	EndLoc.LaneHandle   = FZoneGraphLaneHandle(EndLane,   Storage->DataHandle);
	EndLoc.Position     = MidpointOf(EndLane);

	FZoneGraphAStarWrapper Wrapper(*Storage);
	FZoneGraphAStar AStar(Wrapper);
	FZoneGraphPathFilter Filter(*Storage, StartLoc, EndLoc, TagFilter);
	FZoneGraphAStarNode StartNode(StartLane, StartLoc.Position);
	FZoneGraphAStarNode EndNode(EndLane, EndLoc.Position);

	TArray<int32> PathLanes;
	const EGraphAStarResult AStarResult = AStar.FindPath(StartNode, EndNode, Filter, PathLanes);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("result"),
		AStarResult == EGraphAStarResult::SearchSuccess ? TEXT("SearchSuccess") :
		AStarResult == EGraphAStarResult::GoalUnreachable ? TEXT("GoalUnreachable") :
		AStarResult == EGraphAStarResult::SearchFail ? TEXT("SearchFail") :
		AStarResult == EGraphAStarResult::InfiniteLoop ? TEXT("InfiniteLoop") : TEXT("Unknown"));

	TArray<TSharedPtr<FJsonValue>> LanesArr;
	for (int32 LI : PathLanes)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("data_index"), StartData);
		Obj->SetNumberField(TEXT("lane_index"), LI);
		LanesArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("path_lanes"), LanesArr);
	Result->SetNumberField(TEXT("path_length"), PathLanes.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithZoneGraphActions::HandleListPolygonShapePoints(const TSharedPtr<FJsonObject>& Params)
{
	// Dumps the authored Shape Points (pre-tessellation) of a Polygon ZoneShape so the
	// caller can verify CL-B.5 gate authoring: 4 corner LaneProfile points for the
	// outer vehicle polygon, and 4 for the inner pedestrian polygon, each with the
	// right per-point LaneProfile reference index.
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	const FName Label = MonolithMass::ReadRequiredName(Params, TEXT("actor_label"), Err);
	if (Label.IsNone()) { return FMonolithActionResult::Error(Err); }

	for (TActorIterator<AZoneShape> It(World); It; ++It)
	{
		if (FName(*It->GetActorLabel()) != Label) { continue; }

		const UZoneShapeComponent* Comp = It->GetShape();
		if (!Comp)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AZoneShape '%s' has no UZoneShapeComponent"), *Label.ToString()));
		}
		if (Comp->GetShapeType() != FZoneShapeType::Polygon)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AZoneShape '%s' is Spline type; list_polygon_shape_points requires Polygon"),
				*Label.ToString()));
		}

		TArray<TSharedPtr<FJsonValue>> PointsArr;
		// GetPoints returns TConstArrayView since UE 5.4; cannot bind to a TArray& reference.
		const TConstArrayView<FZoneShapePoint> Points = Comp->GetPoints();
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const FZoneShapePoint& P = Points[i];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("index"), i);
			Obj->SetObjectField(TEXT("position"), MonolithMass::VectorToJson(P.Position));
			// FZoneShapePointType has 4 values (Sharp, Bezier, AutoBezier, LaneProfile) — see
			// ZoneGraphTypes.h line ~770. AutoLaneProfile does not exist.
			const TCHAR* TypeStr = TEXT("Unknown");
			switch (P.Type)
			{
			case FZoneShapePointType::Sharp:       TypeStr = TEXT("Sharp"); break;
			case FZoneShapePointType::Bezier:      TypeStr = TEXT("Bezier"); break;
			case FZoneShapePointType::AutoBezier:  TypeStr = TEXT("AutoBezier"); break;
			case FZoneShapePointType::LaneProfile: TypeStr = TEXT("LaneProfile"); break;
			}
			Obj->SetStringField(TEXT("type"), TypeStr);
			Obj->SetNumberField(TEXT("lane_profile_index"), int32(P.LaneProfile));
			Obj->SetNumberField(TEXT("tangent_length"), P.TangentLength);
			Obj->SetNumberField(TEXT("inner_turn_radius"), P.InnerTurnRadius);
			PointsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		// Expose the component's PerPointLaneProfiles array so callers can resolve the
		// above lane_profile_index → profile name/ID. CommonLaneProfile is the fallback.
		TArray<TSharedPtr<FJsonValue>> PerPointArr;
		for (const FZoneLaneProfileRef& Ref : Comp->GetPerPointLaneProfiles())
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Ref.Name.ToString());
			Obj->SetStringField(TEXT("id"), Ref.ID.ToString());
			PerPointArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		Result->SetNumberField(TEXT("point_count"), Points.Num());
		Result->SetArrayField(TEXT("points"), PointsArr);
		// `GetCommonLaneProfile` is mis-declared non-const in UE 5.7 ZoneShapeComponent.h
		// (line 107). const_cast is the minimal workaround; safe because the method just
		// returns a const ref to a member, no mutation.
		UZoneShapeComponent* MutableComp = const_cast<UZoneShapeComponent*>(Comp);
		Result->SetStringField(TEXT("common_lane_profile_name"),
			MutableComp->GetCommonLaneProfile().Name.ToString());
		Result->SetArrayField(TEXT("per_point_lane_profiles"), PerPointArr);
		Result->SetStringField(TEXT("polygon_routing_type"),
			Comp->GetPolygonRoutingType() == EZoneShapePolygonRoutingType::Bezier ? TEXT("Bezier") :
			Comp->GetPolygonRoutingType() == EZoneShapePolygonRoutingType::Arcs ? TEXT("Arcs") :
			TEXT("None"));
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("AZoneShape with label '%s' not found"), *Label.ToString()));
}

#endif // WITH_ZONEGRAPH
