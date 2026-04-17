// Copyright Matrix Team. All Rights Reserved.

#include "MonolithMassInternal.h"

#include "Engine/World.h"
#include "Editor.h"

namespace MonolithMass
{
	UWorld* GetMcpTargetWorld(FString& OutError)
	{
		if (GEditor)
		{
			// Prefer PIE world — that's where Mass entities / runtime traffic live.
			if (UWorld* PieWorld = GEditor->PlayWorld)
			{
				return PieWorld;
			}
			// Fall back to the Editor's world for authoring-time operations.
			if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
			{
				return EditorWorld;
			}
		}
		OutError = TEXT("No active UWorld (editor not initialised and no PIE session)");
		return nullptr;
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	FName ReadRequiredName(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("Params object missing");
			return NAME_None;
		}
		const FString Str = Params->GetStringField(FieldName);
		if (Str.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required string field '%s'"), *FieldName);
			return NAME_None;
		}
		return FName(*Str);
	}

	bool ReadRequiredInt(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		int32& OutValue,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			OutError = FString::Printf(TEXT("Missing required integer field '%s'"), *FieldName);
			return false;
		}
		double Raw = 0.0;
		if (!Params->TryGetNumberField(FieldName, Raw))
		{
			OutError = FString::Printf(TEXT("Field '%s' is not a number"), *FieldName);
			return false;
		}
		OutValue = FMath::RoundToInt(Raw);
		return true;
	}
}
