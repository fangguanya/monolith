// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithConsoleActions.h"
#include "MonolithParamSchema.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"

namespace
{
	UWorld* GetCommandWorld(bool bPreferPie)
	{
		if (!GEditor) { return nullptr; }
		if (bPreferPie && GEditor->PlayWorld) { return GEditor->PlayWorld; }
		return GEditor->GetEditorWorldContext().World();
	}
}

void FMonolithConsoleActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("console"), TEXT("execute"),
		TEXT("Execute an arbitrary console command (e.g. 'stat fps', 'showflag.zonegraph 1')."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::HandleExecute),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Full console command line"))
			.Optional(TEXT("prefer_pie"), TEXT("boolean"), TEXT("Run against PIE world if active (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("get_cvar"),
		TEXT("Read a CVar's current value as a string."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::HandleGetCVar),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("CVar name (e.g. r.Lumen.Reflections.Allow)"))
			.Build());

	Registry.RegisterAction(TEXT("console"), TEXT("set_cvar"),
		TEXT("Set a CVar's value. The value string is parsed by the CVar's own type-converter."),
		FMonolithActionHandler::CreateStatic(&FMonolithConsoleActions::HandleSetCVar),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("CVar name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("New value (will be coerced)"))
			.Build());
}

FMonolithActionResult FMonolithConsoleActions::HandleExecute(const TSharedPtr<FJsonObject>& Params)
{
	const FString Command = Params->GetStringField(TEXT("command"));
	if (Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Empty 'command'"));
	}

	bool bPreferPie = true;
	if (Params->HasField(TEXT("prefer_pie")))
	{
		bPreferPie = Params->GetBoolField(TEXT("prefer_pie"));
	}

	UWorld* World = GetCommandWorld(bPreferPie);
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	// GEngine->Exec routes to the proper console handler given a world; output is logged but not
	// returned via this API (use editor_query search_logs to capture output).
	const bool bResult = GEngine->Exec(World, *Command);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("command"), Command);
	Result->SetStringField(TEXT("world_type"),
		World->WorldType == EWorldType::PIE ? TEXT("PIE") :
		World->WorldType == EWorldType::Editor ? TEXT("Editor") : TEXT("Other"));
	Result->SetBoolField(TEXT("exec_returned_true"), bResult);
	Result->SetStringField(TEXT("note"),
		TEXT("Use editor_query search_logs to capture command output (Exec does not return text)."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithConsoleActions::HandleGetCVar(const TSharedPtr<FJsonObject>& Params)
{
	const FString Name = Params->GetStringField(TEXT("name"));
	if (Name.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Empty 'name'")); }

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("CVar '%s' not found"), *Name));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("value"), CVar->GetString());
	Result->SetStringField(TEXT("default_value"), CVar->GetDefaultValue());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithConsoleActions::HandleSetCVar(const TSharedPtr<FJsonObject>& Params)
{
	const FString Name = Params->GetStringField(TEXT("name"));
	const FString Value = Params->GetStringField(TEXT("value"));
	if (Name.IsEmpty()) { return FMonolithActionResult::Error(TEXT("Empty 'name'")); }

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("CVar '%s' not found"), *Name));
	}

	CVar->Set(*Value, ECVF_SetByConsole);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("new_value"), CVar->GetString());
	return FMonolithActionResult::Success(Result);
}
