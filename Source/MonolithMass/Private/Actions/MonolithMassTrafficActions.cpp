// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithMassTrafficActions.h"

#if WITH_MASSTRAFFIC
#include "MassTrafficFieldActor.h"
#include "MassTrafficFieldComponent.h"
#include "MassTrafficSettings.h"

#include "ZoneGraphSettings.h"
#include "ZoneGraphTypes.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#endif // WITH_MASSTRAFFIC

void FMonolithMassTrafficActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_MASSTRAFFIC
	Registry.RegisterAction(TEXT("masstraffic"), TEXT("list_field_actors"),
		TEXT("List AMassTrafficFieldActor instances in the current world."),
		FMonolithActionHandler::CreateStatic(&HandleListFieldActors),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("masstraffic"), TEXT("get_field_config"),
		TEXT("Inspect one AMassTrafficFieldActor's field component configuration."),
		FMonolithActionHandler::CreateStatic(&HandleGetFieldConfig),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("Field actor label"))
			.Build());

	Registry.RegisterAction(TEXT("masstraffic"), TEXT("set_field_config"),
		TEXT("Toggle bEnabled and/or set the LaneTagFilter.AnyTags tag on a field actor."),
		FMonolithActionHandler::CreateStatic(&HandleSetFieldConfig),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("Field actor label"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Toggle bEnabled"))
			.Optional(TEXT("include_tag"), TEXT("string"), TEXT("Tag name to add to LaneTagFilter.AnyTags"))
			.Build());

	Registry.RegisterAction(TEXT("masstraffic"), TEXT("rebuild_zone_graph_modifier"),
		TEXT("Find a MassTrafficZoneGraphDataModifier actor and invoke its BuildZoneGraphData UFUNCTION."),
		FMonolithActionHandler::CreateStatic(&HandleRebuildZoneGraphModifier),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("masstraffic"), TEXT("get_settings"),
		TEXT("Summarise UMassTrafficSettings lane filters (TrafficLaneFilter / CrosswalkLaneFilter)."),
		FMonolithActionHandler::CreateStatic(&HandleGetSettings),
		FParamSchemaBuilder().Build());
#else
	(void)Registry;
	UE_LOG(LogMonolithMass, Warning, TEXT("masstraffic.* namespace not registered: WITH_MASSTRAFFIC=0"));
#endif
}

#if WITH_MASSTRAFFIC

FMonolithActionResult FMonolithMassTrafficActions::HandleListFieldActors(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TActorIterator<AMassTrafficFieldActor> It(World); It; ++It)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		if (UMassTrafficFieldComponent* FC = It->GetFieldComponent())
		{
			Entry->SetBoolField(TEXT("enabled"), FC->bEnabled);
		}
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("field_actors"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMassTrafficActions::HandleGetFieldConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	const FName Label = MonolithMass::ReadRequiredName(Params, TEXT("actor_label"), Err);
	if (Label.IsNone()) { return FMonolithActionResult::Error(Err); }

	for (TActorIterator<AMassTrafficFieldActor> It(World); It; ++It)
	{
		if (FName(*It->GetActorLabel()) != Label) { continue; }

		UMassTrafficFieldComponent* FC = It->GetFieldComponent();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		if (FC)
		{
			Result->SetBoolField(TEXT("enabled"), FC->bEnabled);
			Result->SetNumberField(TEXT("any_tags_mask_bits"), FC->LaneTagFilter.AnyTags.GetValue());
			Result->SetNumberField(TEXT("all_tags_mask_bits"), FC->LaneTagFilter.AllTags.GetValue());
			Result->SetNumberField(TEXT("not_tags_mask_bits"), FC->LaneTagFilter.NotTags.GetValue());
			Result->SetObjectField(TEXT("extent"), MonolithMass::VectorToJson(FC->Extent));
		}
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("AMassTrafficFieldActor '%s' not found"), *Label.ToString()));
}

FMonolithActionResult FMonolithMassTrafficActions::HandleSetFieldConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	const FName Label = MonolithMass::ReadRequiredName(Params, TEXT("actor_label"), Err);
	if (Label.IsNone()) { return FMonolithActionResult::Error(Err); }

	for (TActorIterator<AMassTrafficFieldActor> It(World); It; ++It)
	{
		if (FName(*It->GetActorLabel()) != Label) { continue; }

		UMassTrafficFieldComponent* FC = It->GetFieldComponent();
		if (!FC)
		{
			return FMonolithActionResult::Error(TEXT("Field actor has no UMassTrafficFieldComponent"));
		}

		It->Modify();
		FC->Modify();

		if (Params.IsValid() && Params->HasField(TEXT("enabled")))
		{
			FC->bEnabled = Params->GetBoolField(TEXT("enabled"));
		}

		if (Params.IsValid() && Params->HasField(TEXT("include_tag")))
		{
			const FName TagName(*Params->GetStringField(TEXT("include_tag")));
			// Resolve tag name against ZoneGraph settings, then OR it into AnyTags.
			const UZoneGraphSettings* ZGS = GetDefault<UZoneGraphSettings>();
			if (!ZGS)
			{
				return FMonolithActionResult::Error(TEXT("ZoneGraphSettings unavailable"));
			}
			bool bResolved = false;
			for (const FZoneGraphTagInfo& Info : ZGS->GetTagInfos())
			{
				if (Info.Name == TagName)
				{
					FC->LaneTagFilter.AnyTags.Add(Info.Tag);
					bResolved = true;
					break;
				}
			}
			if (!bResolved)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Tag '%s' not declared in ZoneGraphSettings"), *TagName.ToString()));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_label"), It->GetActorLabel());
		Result->SetBoolField(TEXT("enabled"), FC->bEnabled);
		Result->SetNumberField(TEXT("any_tags_mask_bits"), FC->LaneTagFilter.AnyTags.GetValue());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("AMassTrafficFieldActor '%s' not found"), *Label.ToString()));
}

FMonolithActionResult FMonolithMassTrafficActions::HandleRebuildZoneGraphModifier(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	// We intentionally avoid a direct include on MassTrafficZoneGraphDataModifier (which lives in
	// MassTrafficEditor) — walk every actor and invoke its BuildZoneGraphData UFUNCTION by name.
	int32 InvokedCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) { continue; }
		if (!Actor->GetClass()->GetName().Contains(TEXT("MassTrafficZoneGraphDataModifier"))) { continue; }

		if (UFunction* Func = Actor->FindFunction(FName(TEXT("BuildZoneGraphData"))))
		{
			Actor->ProcessEvent(Func, nullptr);
			++InvokedCount;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("invoked_count"), InvokedCount);
	if (InvokedCount == 0)
	{
		Result->SetStringField(TEXT("note"),
			TEXT("No MassTrafficZoneGraphDataModifier actor in world; place one and re-run."));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMassTrafficActions::HandleGetSettings(const TSharedPtr<FJsonObject>& Params)
{
	const UMassTrafficSettings* Settings = GetDefault<UMassTrafficSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UMassTrafficSettings unavailable"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("traffic_lane_filter_any"), Settings->TrafficLaneFilter.AnyTags.GetValue());
	Result->SetNumberField(TEXT("traffic_lane_filter_all"), Settings->TrafficLaneFilter.AllTags.GetValue());
	Result->SetNumberField(TEXT("traffic_lane_filter_not"), Settings->TrafficLaneFilter.NotTags.GetValue());
	Result->SetNumberField(TEXT("crosswalk_lane_filter_any"), Settings->CrosswalkLaneFilter.AnyTags.GetValue());
	Result->SetNumberField(TEXT("crosswalk_lane_filter_not"), Settings->CrosswalkLaneFilter.NotTags.GetValue());
	return FMonolithActionResult::Success(Result);
}

#endif // WITH_MASSTRAFFIC
