// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithBakeActions.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_EDITOR
#include "ZoneGraphData.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneGraphBuilder.h"
#include "NavigationSystem.h"
#endif

void FMonolithBakeActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("bake"), TEXT("zone_graph"),
		TEXT("Invoke FZoneGraphBuilder::BuildAll on every AZoneGraphData in the editor world. "
		     "Convenience alias of zg.build_zone_graph."),
		FMonolithActionHandler::CreateStatic(&FMonolithBakeActions::HandleBakeZoneGraph),
		FParamSchemaBuilder()
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Force full rebuild (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("bake"), TEXT("navmesh"),
		TEXT("Trigger UNavigationSystemV1::Build to rebuild navigation data."),
		FMonolithActionHandler::CreateStatic(&FMonolithBakeActions::HandleBakeNavMesh),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("bake"), TEXT("lighting"),
		TEXT("Kick off Lightmass build (slow — minutes to hours). Returns immediately; check "
		     "editor_query get_build_status for progress."),
		FMonolithActionHandler::CreateStatic(&FMonolithBakeActions::HandleBakeLighting),
		FParamSchemaBuilder()
			.Optional(TEXT("quality"), TEXT("string"), TEXT("Preview/Medium/High/Production (default Preview)"))
			.Build());
}

FMonolithActionResult FMonolithBakeActions::HandleBakeZoneGraph(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	if (!GEditor) { return FMonolithActionResult::Error(TEXT("GEditor unavailable")); }
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) { return FMonolithActionResult::Error(TEXT("No editor world")); }

	UZoneGraphSubsystem* ZGS = UWorld::GetSubsystem<UZoneGraphSubsystem>(World);
	if (!ZGS) { return FMonolithActionResult::Error(TEXT("UZoneGraphSubsystem unavailable")); }

	bool bForce = true;
	if (Params.IsValid() && Params->HasField(TEXT("force")))
	{
		bForce = Params->GetBoolField(TEXT("force"));
	}

	TArray<AZoneGraphData*> DataActors;
	for (TActorIterator<AZoneGraphData> It(World); It; ++It) { DataActors.Add(*It); }
	if (DataActors.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No AZoneGraphData actors in world"));
	}

	ZGS->GetBuilder().BuildAll(DataActors, bForce);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("data_actor_count"), DataActors.Num());
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("bake.zone_graph requires editor build"));
#endif
}

FMonolithActionResult FMonolithBakeActions::HandleBakeNavMesh(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	if (!GEditor) { return FMonolithActionResult::Error(TEXT("GEditor unavailable")); }
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) { return FMonolithActionResult::Error(TEXT("No editor world")); }

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) { return FMonolithActionResult::Error(TEXT("UNavigationSystemV1 unavailable")); }
	NavSys->Build();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("triggered"), true);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("bake.navmesh requires editor build"));
#endif
}

FMonolithActionResult FMonolithBakeActions::HandleBakeLighting(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	if (!GEditor) { return FMonolithActionResult::Error(TEXT("GEditor unavailable")); }
	// GEditor->BuildLighting takes a quality enum; for MVP we trigger via console for safety.
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) { return FMonolithActionResult::Error(TEXT("No editor world")); }
	GEngine->Exec(World, TEXT("BuildLighting"));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("triggered"), true);
	Result->SetStringField(TEXT("note"), TEXT("Lightmass is asynchronous; check editor logs for progress."));
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("bake.lighting requires editor build"));
#endif
}
