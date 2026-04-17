// Copyright Matrix Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithMassLog.h"

class UWorld;

namespace MonolithMass
{
	/**
	 * Resolve the editor's current world (PIE preferred when running, else Editor world).
	 * Every MassEntity / ZoneGraph / MassCrowd query is world-scoped so this is the first
	 * thing every action calls.
	 */
	MONOLITHMASS_API UWorld* GetMcpTargetWorld(FString& OutError);

	/** Helper: FVector → JSON object {x,y,z}. */
	MONOLITHMASS_API TSharedPtr<FJsonObject> VectorToJson(const FVector& V);

	/**
	 * JSON → FName. Returns NAME_None on failure and records why in OutError.
	 * Reads the string field `FieldName` from Params.
	 */
	MONOLITHMASS_API FName ReadRequiredName(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		FString& OutError);

	/**
	 * JSON → int32. Writes `OutValue` and returns true on success; otherwise populates
	 * OutError and returns false. Reads JSON number field `FieldName` from Params.
	 *
	 * Rationale: JSON has a single "number" type, but most MCP callers address lanes /
	 * indices by explicit integer. Rounds to nearest via FMath::RoundToInt to tolerate
	 * callers that encode ints as floats.
	 */
	MONOLITHMASS_API bool ReadRequiredInt(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		int32& OutValue,
		FString& OutError);
}
