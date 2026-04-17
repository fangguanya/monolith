// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithPIEActions.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealEdGlobals.h"

void FMonolithPIEActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("pie"), TEXT("start"),
		TEXT("Start Play-In-Editor in the active Editor viewport."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandleStart),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pie"), TEXT("stop"),
		TEXT("End the current PIE session."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandleStop),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pie"), TEXT("is_active"),
		TEXT("Return whether PIE is currently running (boolean)."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandleIsActive),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pie"), TEXT("pause"),
		TEXT("Pause the PIE world (UGameplayStatics::SetGamePaused(true))."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandlePause),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pie"), TEXT("resume"),
		TEXT("Resume the paused PIE world."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandleResume),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pie"), TEXT("set_time_dilation"),
		TEXT("Multiply the PIE world's time dilation — use 3.0~10.0 to fast-forward tests. "
		     "1.0 to reset."),
		FMonolithActionHandler::CreateStatic(&FMonolithPIEActions::HandleSetTimeDilation),
		FParamSchemaBuilder()
			.Required(TEXT("dilation"), TEXT("number"), TEXT("Time multiplier (1.0 = normal, 10.0 = 10x faster)"))
			.Build());
}

FMonolithActionResult FMonolithPIEActions::HandleStart(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) { return FMonolithActionResult::Error(TEXT("GEditor unavailable")); }
	if (GEditor->PlayWorld) { return FMonolithActionResult::Error(TEXT("PIE already running")); }

	// Simplest reliable mode: launch PIE in the currently selected Editor viewport
	// (equivalent to pressing the Play button).
	FRequestPlaySessionParams SessionParams;
	GEditor->RequestPlaySession(SessionParams);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("requested"), true);
	Result->SetStringField(TEXT("note"),
		TEXT("PIE startup is asynchronous — call pie.is_active after 1-2s to confirm."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPIEActions::HandleStop(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) { return FMonolithActionResult::Error(TEXT("GEditor unavailable")); }
	if (!GEditor->PlayWorld)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("was_active"), false);
		return FMonolithActionResult::Success(Result);
	}
	GEditor->RequestEndPlayMap();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("stopped"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPIEActions::HandleIsActive(const TSharedPtr<FJsonObject>& Params)
{
	const bool bActive = GEditor && GEditor->PlayWorld != nullptr;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_active"), bActive);
	if (bActive && GEditor->PlayWorld)
	{
		Result->SetStringField(TEXT("world_name"), GEditor->PlayWorld->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPIEActions::HandlePause(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not active"));
	}
	UGameplayStatics::SetGamePaused(GEditor->PlayWorld, true);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("paused"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPIEActions::HandleResume(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not active"));
	}
	UGameplayStatics::SetGamePaused(GEditor->PlayWorld, false);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("paused"), false);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPIEActions::HandleSetTimeDilation(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		return FMonolithActionResult::Error(TEXT("PIE not active"));
	}
	double Dilation = 1.0;
	if (!Params->TryGetNumberField(TEXT("dilation"), Dilation))
	{
		return FMonolithActionResult::Error(TEXT("Missing 'dilation' number"));
	}
	Dilation = FMath::Clamp(Dilation, 0.01, 100.0);
	if (AWorldSettings* Settings = GEditor->PlayWorld->GetWorldSettings())
	{
		Settings->SetTimeDilation(static_cast<float>(Dilation));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("dilation"), Dilation);
	return FMonolithActionResult::Success(Result);
}
