#pragma once

#include "CoreMinimal.h"

#if WITH_LOGICDRIVER

// NO direct SM includes — Logic Driver Pro ships precompiled without accessible headers.
// All class access via FindFirstObject<UClass> + reflection (same as MonolithComboGraph).

namespace MonolithLD
{
	// ── Class resolution (cached) ────────────────────────────────
	UClass* GetSMBlueprintClass();           // "SMBlueprint"
	UClass* GetSMNodeBlueprintClass();       // "SMNodeBlueprint"
	UClass* GetSMGraphClass();               // "SMGraph"
	UClass* GetSMGraphNodeBaseClass();       // "SMGraphNode_Base"
	UClass* GetSMGraphNodeStateClass();      // "SMGraphNode_StateNode" or "SMGraphNode_StateNodeBase"
	UClass* GetSMGraphNodeTransitionClass(); // "SMGraphNode_TransitionEdge"
	UClass* GetSMGraphNodeConduitClass();    // "SMGraphNode_ConduitNode"
	UClass* GetSMGraphNodeSMClass();         // "SMGraphNode_StateMachineStateNode"
	UClass* GetSMGraphNodeAnyStateClass();   // "SMGraphNode_AnyStateNode"
	UClass* GetSMBlueprintFactoryClass();    // "SMBlueprintFactory"
	UClass* GetSMNodeBlueprintFactoryClass();// "SMNodeBlueprintFactory"
	UClass* GetSMInstanceClass();            // "SMInstance"
	UClass* GetSMComponentClass();           // "SMStateMachineComponent"

	// ── Helpers ──────────────────────────────────────────────────

	/** Load SM Blueprint from asset path. Returns UBlueprint* (verified as SMBlueprint via class check) */
	UBlueprint* LoadSMBlueprint(const FString& AssetPath, FString& OutError);

	/** Get the root SM graph from a blueprint (first UbergraphPage that is-a SMGraph) */
	UEdGraph* GetRootGraph(UBlueprint* Blueprint);

	/** Find a graph node by GUID string */
	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString);

	/** Compile an SM Blueprint */
	bool CompileSMBlueprint(UBlueprint* Blueprint, FString& OutError);

	/** Determine node type string from a graph node's class */
	FString GetNodeType(UEdGraphNode* Node);

	/** Serialize a graph node to JSON */
	TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, bool bDetailed = false);

	/** Serialize full SM structure to JSON */
	TSharedPtr<FJsonObject> SMStructureToJson(UBlueprint* Blueprint, int32 MaxDepth = -1);

	// ── Reflection utilities (copied from ComboGraph pattern) ───

	template<typename T>
	T* GetPropertyPtr(UObject* Obj, FName PropName)
	{
		if (!Obj) return nullptr;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropName);
		if (!Prop) return nullptr;
		return Prop->ContainerPtrToValuePtr<T>(Obj);
	}

	UObject* GetObjectProperty(UObject* Obj, FName PropName);
	FString GetStringProperty(UObject* Obj, FName PropName);
	bool GetBoolProperty(UObject* Obj, FName PropName, bool Default = false);
	int32 GetIntProperty(UObject* Obj, FName PropName, int32 Default = 0);
	float GetFloatProperty(UObject* Obj, FName PropName, float Default = 0.f);
}

#endif // WITH_LOGICDRIVER
