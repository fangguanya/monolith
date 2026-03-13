#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintNodeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleAddNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConnectPins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDisconnectPins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetPinDefault(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetNodePosition(const TSharedPtr<FJsonObject>& Params);
};
