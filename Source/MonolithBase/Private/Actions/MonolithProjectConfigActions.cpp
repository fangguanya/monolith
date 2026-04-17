// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithProjectConfigActions.h"
#include "MonolithParamSchema.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/**
	 * Resolve a logical ini name (e.g. "DefaultPlugins" / "DefaultEngine") to the canonical
	 * runtime config name UE uses (`Plugins` / `Engine`). UE ini machinery names sections by
	 * stripping the "Default" prefix and removing ".ini" — `DefaultPlugins.ini` → `Plugins`.
	 */
	FString ResolveIniLogicalName(const FString& InFile)
	{
		FString Name = InFile;
		if (Name.EndsWith(TEXT(".ini"))) { Name = Name.LeftChop(4); }
		if (Name.StartsWith(TEXT("Default"))) { Name = Name.RightChop(7); }
		return Name;
	}

	/** Resolve `DefaultPlugins.ini` → `<ProjectDir>/Config/DefaultPlugins.ini` for write paths. */
	FString ResolveDefaultIniPath(const FString& InFile)
	{
		FString FileName = InFile;
		if (!FileName.EndsWith(TEXT(".ini"))) { FileName += TEXT(".ini"); }
		if (!FileName.StartsWith(TEXT("Default"))) { FileName = TEXT("Default") + FileName; }
		return FPaths::ProjectConfigDir() / FileName;
	}
}

void FMonolithProjectConfigActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("project"), TEXT("read_config"),
		TEXT("Read a value from a Default*.ini section/key. Looks at the merged runtime config "
		     "(includes platform overlays). For raw on-disk file content use source.read_file."),
		FMonolithActionHandler::CreateStatic(&FMonolithProjectConfigActions::HandleReadConfig),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Logical ini name e.g. 'DefaultPlugins' or 'Plugins'"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Section header e.g. /Script/MassTraffic.MassTrafficSettings"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Key name e.g. TrafficLaneFilter"))
			.Build());

	Registry.RegisterAction(TEXT("project"), TEXT("write_config"),
		TEXT("Set a value in a Default*.ini. Writes through GConfig (will p4 edit if needed). "
		     "Caller is responsible for reload_config or restart."),
		FMonolithActionHandler::CreateStatic(&FMonolithProjectConfigActions::HandleWriteConfig),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Logical ini name"))
			.Required(TEXT("section"), TEXT("string"), TEXT("Section header"))
			.Required(TEXT("key"), TEXT("string"), TEXT("Key name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as string"))
			.Build());

	Registry.RegisterAction(TEXT("project"), TEXT("reload_config"),
		TEXT("Call UObject::ReloadConfig on a named UCLASS so its DefaultConfig changes apply "
		     "without an editor restart. Not all settings classes support hot reload."),
		FMonolithActionHandler::CreateStatic(&FMonolithProjectConfigActions::HandleReloadConfig),
		FParamSchemaBuilder()
			.Required(TEXT("class_name"), TEXT("string"), TEXT("UCLASS name e.g. UMassCrowdSettings"))
			.Build());
}

FMonolithActionResult FMonolithProjectConfigActions::HandleReadConfig(const TSharedPtr<FJsonObject>& Params)
{
	const FString File = Params->GetStringField(TEXT("file"));
	const FString Section = Params->GetStringField(TEXT("section"));
	const FString Key = Params->GetStringField(TEXT("key"));
	if (File.IsEmpty() || Section.IsEmpty() || Key.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing file/section/key"));
	}

	const FString ConfigName = ResolveIniLogicalName(File);
	FString Value;
	const bool bFound = GConfig->GetString(*Section, *Key, Value, ConfigName);
	if (!bFound)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Key '%s.%s' not found in '%s'"), *Section, *Key, *ConfigName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file"), ConfigName);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("value"), Value);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithProjectConfigActions::HandleWriteConfig(const TSharedPtr<FJsonObject>& Params)
{
	const FString File = Params->GetStringField(TEXT("file"));
	const FString Section = Params->GetStringField(TEXT("section"));
	const FString Key = Params->GetStringField(TEXT("key"));
	const FString Value = Params->GetStringField(TEXT("value"));
	if (File.IsEmpty() || Section.IsEmpty() || Key.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing file/section/key"));
	}

	const FString ConfigName = ResolveIniLogicalName(File);
	const FString DiskPath = ResolveDefaultIniPath(File);
	GConfig->SetString(*Section, *Key, *Value, ConfigName);
	GConfig->Flush(/*bRemoveFromCache*/ false, ConfigName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file"), ConfigName);
	Result->SetStringField(TEXT("disk_path"), DiskPath);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("note"),
		TEXT("GConfig persisted; caller may need project.reload_config or editor restart for the "
		     "owning UCLASS to pick up the change."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithProjectConfigActions::HandleReloadConfig(const TSharedPtr<FJsonObject>& Params)
{
	const FString ClassName = Params->GetStringField(TEXT("class_name"));
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing 'class_name'"));
	}

	UClass* TargetClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ClassName)
		{
			TargetClass = *It;
			break;
		}
	}
	if (!TargetClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("UCLASS '%s' not found"), *ClassName));
	}

	UObject* CDO = TargetClass->GetDefaultObject();
	if (!CDO)
	{
		return FMonolithActionResult::Error(TEXT("UCLASS has no CDO"));
	}
	CDO->ReloadConfig();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), ClassName);
	Result->SetBoolField(TEXT("reloaded"), true);
	return FMonolithActionResult::Success(Result);
}
