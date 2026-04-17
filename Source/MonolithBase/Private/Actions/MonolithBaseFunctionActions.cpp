// Copyright Matrix Team. All Rights Reserved.

#include "Actions/MonolithBaseFunctionActions.h"
#include "MonolithParamSchema.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"

namespace
{
	UWorld* GetEditorOrPieWorld()
	{
		if (!GEditor) { return nullptr; }
		if (UWorld* PieWorld = GEditor->PlayWorld) { return PieWorld; }
		return GEditor->GetEditorWorldContext().World();
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		if (!World || Label.IsEmpty()) { return nullptr; }
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * Invoke a UFUNCTION by name with no parameters.
	 * For MVP we support only no-arg UFUNCTIONs (which covers BakeAll / ClearGenerated /
	 * SpawnAllMassActors / BuildZoneGraphData — the current critical path). Parametered calls
	 * can be added later without breaking this API.
	 */
	FMonolithActionResult InvokeImpl(const TSharedPtr<FJsonObject>& Params, bool bRequireCallInEditor)
	{
		UWorld* World = GetEditorOrPieWorld();
		if (!World)
		{
			return FMonolithActionResult::Error(TEXT("No active UWorld"));
		}

		const FString Label = Params->GetStringField(TEXT("actor_label"));
		const FString FuncNameStr = Params->GetStringField(TEXT("function_name"));
		if (Label.IsEmpty() || FuncNameStr.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required 'actor_label' or 'function_name'"));
		}

		AActor* Actor = FindActorByLabel(World, Label);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Actor with label '%s' not found"), *Label));
		}

		UFunction* Func = Actor->FindFunction(FName(*FuncNameStr));
		if (!Func)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("UFUNCTION '%s' not found on '%s' (%s)"),
				*FuncNameStr, *Label, *Actor->GetClass()->GetName()));
		}

		if (bRequireCallInEditor && !Func->GetBoolMetaData(TEXT("CallInEditor")))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("UFUNCTION '%s' is not marked CallInEditor — refused"), *FuncNameStr));
		}

		// MVP rule: 0 INPUT params allowed; a single return value is fine — UE counts return as
		// a parm so we allow NumParms<=1 only when the lone parm is the return value.
		const bool bHasReturn = (Func->ReturnValueOffset != MAX_uint16);
		const int32 InputParms = Func->NumParms - (bHasReturn ? 1 : 0);
		if (InputParms > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("UFUNCTION '%s' has %d input param(s) — only 0-input calls are supported in this MVP"),
				*FuncNameStr, InputParms));
		}

		// Allocate ParmsSize bytes (zero-initialised) — covers the return-value slot when present.
		// Stack alloca is sized via the function's reflected ParmsSize so we don't over/under-shoot.
		uint8* ParmBuffer = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(Func->ParmsSize, 1)));
		FMemory::Memzero(ParmBuffer, Func->ParmsSize);
		Actor->ProcessEvent(Func, ParmBuffer);

		// Surface the return value as JSON if the function had one (int32 / float / FString covered).
		TSharedPtr<FJsonObject> ReturnJson;
		if (bHasReturn)
		{
			if (FProperty* ReturnProp = Func->GetReturnProperty())
			{
				ReturnJson = MakeShared<FJsonObject>();
				ReturnJson->SetStringField(TEXT("type"), ReturnProp->GetCPPType());
				FString ValueStr;
				ReturnProp->ExportTextItem_Direct(ValueStr,
					ParmBuffer + ReturnProp->GetOffset_ForUFunction(),
					nullptr, nullptr, PPF_None);
				ReturnJson->SetStringField(TEXT("value"), ValueStr);
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor_label"), Label);
		Result->SetStringField(TEXT("function_name"), FuncNameStr);
		Result->SetBoolField(TEXT("invoked"), true);
		Result->SetBoolField(TEXT("call_in_editor_verified"), bRequireCallInEditor);
		if (ReturnJson.IsValid())
		{
			Result->SetObjectField(TEXT("return_value"), ReturnJson);
		}
		return FMonolithActionResult::Success(Result);
	}
}

void FMonolithBaseFunctionActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("base"), TEXT("invoke_function"),
		TEXT("Invoke a UFUNCTION (0-param) on an Actor located by label. Fires PostEdit / "
		     "any impure side-effects the function declares."),
		FMonolithActionHandler::CreateStatic(&FMonolithBaseFunctionActions::HandleInvokeFunction),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("Target actor's label"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("UFUNCTION name (must be 0-param in this MVP)"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("call_in_editor_function"),
		TEXT("Safer variant: only invokes if the UFUNCTION is marked meta=(CallInEditor) — "
		     "suitable for automation scripts that should refuse to run runtime-only functions."),
		FMonolithActionHandler::CreateStatic(&FMonolithBaseFunctionActions::HandleCallInEditorFunction),
		FParamSchemaBuilder()
			.Required(TEXT("actor_label"), TEXT("string"), TEXT("Target actor's label"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("UFUNCTION name"))
			.Build());
}

FMonolithActionResult FMonolithBaseFunctionActions::HandleInvokeFunction(const TSharedPtr<FJsonObject>& Params)
{
	return InvokeImpl(Params, /*bRequireCallInEditor*/ false);
}

FMonolithActionResult FMonolithBaseFunctionActions::HandleCallInEditorFunction(const TSharedPtr<FJsonObject>& Params)
{
	return InvokeImpl(Params, /*bRequireCallInEditor*/ true);
}
