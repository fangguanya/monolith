// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithMassCrowdActions.h"

#if WITH_MASSCROWD
#include "MassCrowdSettings.h"
#include "MassCrowdSubsystem.h"
#include "MassSpawner.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#endif // WITH_MASSCROWD

void FMonolithMassCrowdActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_MASSCROWD
	Registry.RegisterAction(TEXT("masscrowd"), TEXT("list_spawners"),
		TEXT("List AMassSpawner actors tagged or named 'Crowd'."),
		FMonolithActionHandler::CreateStatic(&HandleListSpawners),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("masscrowd"), TEXT("get_settings"),
		TEXT("Return UMassCrowdSettings CrowdTag / CrossingTag bit indices."),
		FMonolithActionHandler::CreateStatic(&HandleGetSettings),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("masscrowd"), TEXT("get_lane_density"),
		TEXT("Dump UMassCrowdSettings::GetLaneDensities — per-tag density weight."),
		FMonolithActionHandler::CreateStatic(&HandleGetLaneDensity),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("masscrowd"), TEXT("rebuild_lane_data"),
		TEXT("Invoke UMassCrowdSubsystem::RebuildLaneData (editor only)."),
		FMonolithActionHandler::CreateStatic(&HandleRebuildLaneData),
		FParamSchemaBuilder().Build());
#else
	(void)Registry;
	UE_LOG(LogMonolithMass, Warning, TEXT("masscrowd.* namespace not registered: WITH_MASSCROWD=0"));
#endif
}

#if WITH_MASSCROWD

FMonolithActionResult FMonolithMassCrowdActions::HandleListSpawners(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TActorIterator<AMassSpawner> It(World); It; ++It)
	{
		AMassSpawner* Spawner = *It;
		// Heuristic "is it a crowd spawner?" — actor label / class name contains "Crowd"
		// or actor is tagged "Crowd". Keeps the tool useful before entity config introspection
		// is hooked up; subsequent CLs can tighten this.
		const bool bIsCrowdLabeled = Spawner->GetActorLabel().Contains(TEXT("Crowd"));
		const bool bIsCrowdClass = Spawner->GetClass()->GetName().Contains(TEXT("Crowd"));
		const bool bHasCrowdTag = Spawner->Tags.Contains(TEXT("Crowd"));
		if (!(bIsCrowdLabeled || bIsCrowdClass || bHasCrowdTag))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor_label"), Spawner->GetActorLabel());
		Entry->SetStringField(TEXT("class"), Spawner->GetClass()->GetName());
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("spawners"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMassCrowdActions::HandleGetSettings(const TSharedPtr<FJsonObject>& Params)
{
	const UMassCrowdSettings* Settings = GetDefault<UMassCrowdSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UMassCrowdSettings unavailable"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("crowd_tag_bit"), Settings->CrowdTag.Get());
	Result->SetNumberField(TEXT("crossing_tag_bit"), Settings->CrossingTag.Get());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMassCrowdActions::HandleGetLaneDensity(const TSharedPtr<FJsonObject>& Params)
{
	const UMassCrowdSettings* Settings = GetDefault<UMassCrowdSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("UMassCrowdSettings unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FMassCrowdLaneDensityDesc& Desc : Settings->GetLaneDensities())
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("tag_bit"), Desc.Tag.Get());
		Entry->SetNumberField(TEXT("weight"), Desc.Weight);
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("densities"), Arr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMassCrowdActions::HandleRebuildLaneData(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString Err;
	UWorld* World = MonolithMass::GetMcpTargetWorld(Err);
	if (!World) { return FMonolithActionResult::Error(Err); }

	UMassCrowdSubsystem* Sub = UWorld::GetSubsystem<UMassCrowdSubsystem>(World);
	if (!Sub)
	{
		return FMonolithActionResult::Error(TEXT("UMassCrowdSubsystem unavailable in this world"));
	}

	Sub->RebuildLaneData();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("rebuilt"), true);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("rebuild_lane_data requires an editor build"));
#endif
}

#endif // WITH_MASSCROWD
