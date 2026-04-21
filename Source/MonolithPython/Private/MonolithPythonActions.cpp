#include "MonolithPythonActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithPythonActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("python"), TEXT("execute"),
		TEXT("Execute Python code in the UE Editor with captured stdout/stderr. Multi-statement scripts OK. Uses EPythonCommandExecutionMode::ExecuteFile."),
		FMonolithActionHandler::CreateStatic(&FMonolithPythonActions::Execute),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Python source code (may contain multiple statements)"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("'private' (default, isolated globals) or 'public' (shared with console)"))
			.Optional(TEXT("unattended"), TEXT("boolean"), TEXT("Suppress UI dialogs (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("python"), TEXT("execute_file"),
		TEXT("Read a .py file from disk and execute it inside the UE Editor. Use for longer scripts to avoid JSON-escaping."),
		FMonolithActionHandler::CreateStatic(&FMonolithPythonActions::ExecuteFile),
		FParamSchemaBuilder()
			.Required(TEXT("path"), TEXT("string"), TEXT("Absolute path to a .py file"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("'private' (default) or 'public'"))
			.Optional(TEXT("unattended"), TEXT("boolean"), TEXT("Default true"))
			.Build());

	Registry.RegisterAction(TEXT("python"), TEXT("evaluate"),
		TEXT("Evaluate a single Python expression and return its repr. Cannot run statements or files."),
		FMonolithActionHandler::CreateStatic(&FMonolithPythonActions::Evaluate),
		FParamSchemaBuilder()
			.Required(TEXT("expression"), TEXT("string"), TEXT("Single Python expression (e.g. '1+1', 'unreal.SystemLibrary.get_engine_version()')"))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

namespace
{
	bool GetBoolFieldOr(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default)
	{
		if (!Params.IsValid()) return Default;
		bool OutValue = Default;
		if (Params->TryGetBoolField(Field, OutValue)) return OutValue;
		return Default;
	}

	EPythonFileExecutionScope ResolveScope(const TSharedPtr<FJsonObject>& Params)
	{
		FString ScopeStr;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("scope"), ScopeStr))
		{
			if (ScopeStr.Equals(TEXT("public"), ESearchCase::IgnoreCase))
			{
				return EPythonFileExecutionScope::Public;
			}
		}
		return EPythonFileExecutionScope::Private;
	}

	const TCHAR* LogTypeToString(EPythonLogOutputType Type)
	{
		switch (Type)
		{
		case EPythonLogOutputType::Info:    return TEXT("info");
		case EPythonLogOutputType::Warning: return TEXT("warning");
		case EPythonLogOutputType::Error:   return TEXT("error");
		default:                            return TEXT("unknown");
		}
	}

	/** Build the structured result object shared by all three actions. */
	TSharedPtr<FJsonObject> BuildResult(const FPythonCommandEx& Cmd, bool bOk, double ElapsedMs)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("success"), bOk);
		Obj->SetStringField(TEXT("command_result"), Cmd.CommandResult);
		Obj->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);

		TArray<TSharedPtr<FJsonValue>> LogArray;
		FString CombinedInfo;
		FString CombinedWarning;
		FString CombinedError;
		for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
		{
			auto EntryJson = MakeShared<FJsonObject>();
			EntryJson->SetStringField(TEXT("type"), LogTypeToString(Entry.Type));
			EntryJson->SetStringField(TEXT("text"), Entry.Output);
			LogArray.Add(MakeShared<FJsonValueObject>(EntryJson));

			switch (Entry.Type)
			{
			case EPythonLogOutputType::Info:    CombinedInfo    += Entry.Output; break;
			case EPythonLogOutputType::Warning: CombinedWarning += Entry.Output; break;
			case EPythonLogOutputType::Error:   CombinedError   += Entry.Output; break;
			}
		}
		Obj->SetArrayField(TEXT("log_output"), LogArray);
		Obj->SetStringField(TEXT("stdout"), CombinedInfo);
		Obj->SetStringField(TEXT("stderr"), CombinedError);
		Obj->SetStringField(TEXT("warnings"), CombinedWarning);
		Obj->SetNumberField(TEXT("log_count"), Cmd.LogOutput.Num());
		return Obj;
	}

	/** Check python plugin availability and return a pre-built error result if not ready. */
	TOptional<FMonolithActionResult> CheckPythonReady()
	{
		IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
		if (!Plugin)
		{
			return FMonolithActionResult::Error(TEXT("PythonScriptPlugin is not loaded. Ensure the engine plugin is enabled."));
		}
		if (!Plugin->IsPythonAvailable())
		{
			return FMonolithActionResult::Error(TEXT("Python support is disabled. Enable PythonScriptPlugin and restart the editor."));
		}
		if (!Plugin->IsPythonInitialized())
		{
			return FMonolithActionResult::Error(TEXT("Python is not initialized yet. Wait for editor startup to finish and retry."));
		}
		return TOptional<FMonolithActionResult>();
	}
}

// ============================================================================
// Action: python.execute
// Params: { "command": "...", "scope": "private|public", "unattended": bool }
// ============================================================================

FMonolithActionResult FMonolithPythonActions::Execute(const TSharedPtr<FJsonObject>& Params)
{
	if (auto NotReady = CheckPythonReady()) return NotReady.GetValue();

	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty 'command' parameter."));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = Command;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Cmd.FileExecutionScope = ResolveScope(Params);
	Cmd.Flags = GetBoolFieldOr(Params, TEXT("unattended"), true)
		? EPythonCommandFlags::Unattended
		: EPythonCommandFlags::None;

	const double StartSec = FPlatformTime::Seconds();
	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

	return FMonolithActionResult::Success(BuildResult(Cmd, bOk, ElapsedMs));
}

// ============================================================================
// Action: python.execute_file
// Params: { "path": "C:/.../script.py", "scope": "private|public", "unattended": bool }
// ============================================================================

FMonolithActionResult FMonolithPythonActions::ExecuteFile(const TSharedPtr<FJsonObject>& Params)
{
	if (auto NotReady = CheckPythonReady()) return NotReady.GetValue();

	FString Path;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty 'path' parameter."));
	}

	FPaths::NormalizeFilename(Path);
	if (!FPaths::FileExists(Path))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("File not found: %s"), *Path));
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *Path))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to read file: %s"), *Path));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = FileContents;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Cmd.FileExecutionScope = ResolveScope(Params);
	Cmd.Flags = GetBoolFieldOr(Params, TEXT("unattended"), true)
		? EPythonCommandFlags::Unattended
		: EPythonCommandFlags::None;

	const double StartSec = FPlatformTime::Seconds();
	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

	TSharedPtr<FJsonObject> Result = BuildResult(Cmd, bOk, ElapsedMs);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("source_length"), FileContents.Len());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Action: python.evaluate
// Params: { "expression": "..." }
// ============================================================================

FMonolithActionResult FMonolithPythonActions::Evaluate(const TSharedPtr<FJsonObject>& Params)
{
	if (auto NotReady = CheckPythonReady()) return NotReady.GetValue();

	FString Expression;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("expression"), Expression) || Expression.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty 'expression' parameter."));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = Expression;
	Cmd.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	Cmd.FileExecutionScope = EPythonFileExecutionScope::Private;
	Cmd.Flags = EPythonCommandFlags::Unattended;

	const double StartSec = FPlatformTime::Seconds();
	const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

	TSharedPtr<FJsonObject> Result = BuildResult(Cmd, bOk, ElapsedMs);
	Result->SetStringField(TEXT("expression"), Expression);
	return FMonolithActionResult::Success(Result);
}
