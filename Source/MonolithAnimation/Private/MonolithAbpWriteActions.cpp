#include "MonolithAbpWriteActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
// Wave 11 — expanded whitelist for ABP write
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_Fabrik.h"
#include "AnimGraphNode_CopyBone.h"
#include "AnimGraphNode_LookAt.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimStateNode.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// PoseSearchEditor module — provides UAnimGraphNode_MotionMatching
#include "AnimGraphNode_MotionMatching.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAbpWriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- add_anim_graph_node ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_anim_graph_node"),
		TEXT("Place an animation graph node (SequencePlayer, BlendSpacePlayer, TwoWayBlend, BlendListByBool, LayeredBoneBlend, MotionMatching) in a state or the main AnimGraph"),
		FMonolithActionHandler::CreateStatic(&HandleAddAnimGraphNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_type"), TEXT("string"), TEXT("Node type: SequencePlayer, BlendSpacePlayer, TwoWayBlend, BlendListByBool, LayeredBoneBlend, MotionMatching"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph name — 'AnimGraph' for top-level, or a state name for state inner graphs (default: AnimGraph)"), TEXT("AnimGraph"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("State name — if set, node is placed inside this state's inner graph (searched within the state machine found via graph_name if graph_name is a SM name, otherwise searches all SMs)"))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Optional(TEXT("anim_asset"), TEXT("string"), TEXT("Animation/BlendSpace asset path — for SequencePlayer and BlendSpacePlayer nodes"))
			.Build());

	// --- connect_anim_graph_pins ---
	Registry.RegisterAction(TEXT("animation"), TEXT("connect_anim_graph_pins"),
		TEXT("Wire two node pins together in an ABP anim graph. Use after add_anim_graph_node to connect pose outputs to inputs."),
		FMonolithActionHandler::CreateStatic(&HandleConnectAnimGraphPins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("source_node"), TEXT("string"), TEXT("Source node name (UObject name from add_anim_graph_node response, or class-based like AnimGraphNode_SequencePlayer_0)"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source pin name, e.g. 'Pose' (output pin)"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Target node name"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target pin name, e.g. 'Result', 'A', 'B', 'BlendPose_0'"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name to search in (default: searches all graphs)"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("State name to search in — narrows to a specific state's inner graph"))
			.Optional(TEXT("compile"), TEXT("bool"), TEXT("Compile ABP after wiring (default: true)"), TEXT("true"))
			.Build());

	// --- set_state_animation ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_state_animation"),
		TEXT("High-level shortcut: set which animation a state plays by spawning the right player node and wiring it to the state result. Handles SequencePlayer vs BlendSpacePlayer automatically."),
		FMonolithActionHandler::CreateStatic(&HandleSetStateAnimation),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name (as shown in get_state_machines)"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State name to set animation for"))
			.Required(TEXT("anim_asset_path"), TEXT("string"), TEXT("AnimSequence or BlendSpace asset path"))
			.Optional(TEXT("loop"), TEXT("bool"), TEXT("Set loop flag on the player node"), TEXT("false"))
			.Optional(TEXT("clear_existing"), TEXT("bool"), TEXT("Remove existing animation nodes wired to the state result (default: true)"), TEXT("true"))
			.Build());

	// --- set_linked_layer --- (Wave 12)
	Registry.RegisterAction(TEXT("animation"), TEXT("set_linked_layer"),
		TEXT("Configure a LinkedAnimLayer node: set which interface layer function it points to. "
			"Call after add_anim_graph_node with node_type=LinkedAnimLayer."),
		FMonolithActionHandler::CreateStatic(&HandleSetLinkedLayer),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("Node UObject name (from add_anim_graph_node response)"))
			.Required(TEXT("layer_name"), TEXT("string"), TEXT("Layer function name (e.g. 'FullBody_Aiming', 'FullBody_SkeletalControls')"))
			.Optional(TEXT("compile"), TEXT("bool"), TEXT("Compile ABP after change (default: true)"), TEXT("true"))
			.Build());

	// --- set_anim_node_property --- (Wave 12)
	Registry.RegisterAction(TEXT("animation"), TEXT("set_anim_node_property"),
		TEXT("Set a property on an anim graph node's inner FAnimNode struct via UE reflection. "
			"Supports bool, int, float, FName, FString, enum (by name), and FBoneReference (bone_name sub-field)."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimNodeProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("Node UObject name (from add_anim_graph_node response or get_graph_data)"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name on the FAnimNode struct (e.g. 'BoneToModify', 'TranslationMode', 'RotationMode')"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as string. For enums use display name (e.g. 'Replace Existing'). For FBoneReference pass the bone name directly."))
			.Optional(TEXT("compile"), TEXT("bool"), TEXT("Compile ABP after change (default: false)"), TEXT("false"))
			.Build());

	// --- set_property_binding --- (Wave 13)
	Registry.RegisterAction(TEXT("animation"), TEXT("set_property_binding"),
		TEXT("Bind an anim graph node pin to an AnimInstance property via UE5 Property Access. "
			"This is the programmatic equivalent of right-clicking a pin in the ABP editor and selecting "
			"'Bind' → picking a property. Works for any pin on ModifyBone, LayeredBoneBlend, etc. "
			"The property must be accessible from the AnimInstance context (including CPP parent fields)."),
		FMonolithActionHandler::CreateStatic(&HandleSetPropertyBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_name"), TEXT("string"), TEXT("Node UObject name"))
			.Required(TEXT("binding_name"), TEXT("string"), TEXT("Binding name — usually the pin/property name (e.g. 'Alpha', 'Translation')"))
			.Required(TEXT("property_path"), TEXT("string"), TEXT("Property path on AnimInstance to bind to (e.g. 'HandIK_Right_Alpha')"))
			.Optional(TEXT("compile"), TEXT("bool"), TEXT("Compile ABP after change (default: true)"), TEXT("true"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/** Map a user-facing node type string to UClass. Returns nullptr on unknown type. */
UClass* ResolveNodeClass(const FString& NodeType)
{
	// Original Wave 7 types
	if (NodeType.Equals(TEXT("SequencePlayer"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_SequencePlayer::StaticClass();
	if (NodeType.Equals(TEXT("BlendSpacePlayer"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_BlendSpacePlayer::StaticClass();
	if (NodeType.Equals(TEXT("TwoWayBlend"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_TwoWayBlend::StaticClass();
	if (NodeType.Equals(TEXT("BlendListByBool"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_BlendListByBool::StaticClass();
	if (NodeType.Equals(TEXT("LayeredBoneBlend"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LayeredBoneBlend::StaticClass();
	if (NodeType.Equals(TEXT("MotionMatching"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_MotionMatching::StaticClass();
	// Wave 11 — Link / Post-process layer nodes
	if (NodeType.Equals(TEXT("LinkedAnimLayer"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LinkedAnimLayer::StaticClass();
	if (NodeType.Equals(TEXT("LinkedAnimGraph"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LinkedAnimGraph::StaticClass();
	// Wave 11 — Skeletal Control nodes (IK / bone modification)
	if (NodeType.Equals(TEXT("ModifyBone"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_ModifyBone::StaticClass();
	if (NodeType.Equals(TEXT("TwoBoneIK"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_TwoBoneIK::StaticClass();
	if (NodeType.Equals(TEXT("Fabrik"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_Fabrik::StaticClass();
	if (NodeType.Equals(TEXT("CopyBone"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_CopyBone::StaticClass();
	if (NodeType.Equals(TEXT("LookAt"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LookAt::StaticClass();
	// Wave 11 — Utility nodes
	if (NodeType.Equals(TEXT("Slot"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_Slot::StaticClass();
	if (NodeType.Equals(TEXT("SaveCachedPose"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_SaveCachedPose::StaticClass();
	return nullptr;
}

/** Find a state machine graph by its display title (same lookup as Wave 10 add_state_to_machine). */
UAnimationStateMachineGraph* FindSMGraphByName(UAnimBlueprint* ABP, const FString& MachineName)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (SMTitle == MachineName)
			{
				return Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			}
		}
	}
	return nullptr;
}

/** Find a state node by name within a state machine graph. */
UAnimStateNode* FindStateByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
		if (StateNode && StateNode->GetStateName() == StateName)
		{
			return StateNode;
		}
	}
	return nullptr;
}

/**
 * Resolve the target graph from graph_name and state_name parameters.
 * - If state_name is provided, searches all state machines for that state and returns its inner graph.
 * - If graph_name is "AnimGraph", returns the top-level AnimGraph.
 * - Otherwise treats graph_name as a state machine name and looks for state_name within it.
 */
UEdGraph* ResolveTargetGraph(UAnimBlueprint* ABP, const FString& GraphName, const FString& StateName, FString& OutError)
{
	// If state_name is specified, find the state and return its inner graph
	if (!StateName.IsEmpty())
	{
		// Search all state machines for this state
		for (UEdGraph* Graph : ABP->FunctionGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
				if (!SMNode) continue;

				UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
				if (!SMGraph) continue;

				UAnimStateNode* StateNode = FindStateByName(SMGraph, StateName);
				if (StateNode)
				{
					UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
					if (!StateGraph)
					{
						OutError = FString::Printf(TEXT("State '%s' has no inner animation graph (BoundGraph is null)"), *StateName);
						return nullptr;
					}
					return StateGraph;
				}
			}
		}
		OutError = FString::Printf(TEXT("State '%s' not found in any state machine"), *StateName);
		return nullptr;
	}

	// No state_name — use graph_name
	if (GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase) || GraphName.IsEmpty())
	{
		// Find the main AnimGraph (first UAnimationGraph in FunctionGraphs)
		for (UEdGraph* Graph : ABP->FunctionGraphs)
		{
			if (UAnimationGraph* AG = Cast<UAnimationGraph>(Graph))
			{
				return AG;
			}
		}
		OutError = TEXT("No AnimGraph found in this Animation Blueprint");
		return nullptr;
	}

	// Treat graph_name as a named function graph
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	OutError = FString::Printf(TEXT("Graph '%s' not found. Use 'AnimGraph' for the main graph, or provide state_name to target a state's inner graph."), *GraphName);
	return nullptr;
}

/** Find a node by UObject name across all graphs in an ABP, or within a specific graph. */
UEdGraphNode* FindNodeByName(UAnimBlueprint* ABP, const FString& NodeName, UEdGraph* InGraph = nullptr)
{
	auto SearchGraph = [&](UEdGraph* Graph) -> UEdGraphNode*
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Node;
			}
		}
		return nullptr;
	};

	if (InGraph)
	{
		return SearchGraph(InGraph);
	}

	// Search all function graphs and their subgraphs
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (UEdGraphNode* Found = SearchGraph(Graph))
			return Found;

		// Search inside state machine graphs
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			if (UEdGraphNode* Found = SearchGraph(SMGraph))
				return Found;

			// Search inside each state's inner graph
			for (UEdGraphNode* SMChild : SMGraph->Nodes)
			{
				UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChild);
				if (!StateNode || !StateNode->BoundGraph) continue;

				if (UEdGraphNode* Found = SearchGraph(StateNode->BoundGraph))
					return Found;
			}
		}
	}
	return nullptr;
}

/** Build a JSON array describing a node's pins. */
TArray<TSharedPtr<FJsonValue>> BuildPinList(UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PinObj->SetBoolField(TEXT("is_pose"), UAnimationGraphSchema::IsPosePin(Pin->PinType));
		PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
		PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

		PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	return PinsArr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Action: add_anim_graph_node
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeType  = Params->GetStringField(TEXT("node_type"));
	FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
	FString StateName = Params->HasField(TEXT("state_name")) ? Params->GetStringField(TEXT("state_name")) : TEXT("");
	FString AnimAsset = Params->HasField(TEXT("anim_asset")) ? Params->GetStringField(TEXT("anim_asset")) : TEXT("");

	double TempVal;
	float PosX = 200.f;
	float PosY = 0.f;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<float>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<float>(TempVal);

	if (NodeType.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: node_type"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Resolve the node class
	UClass* NodeClass = ResolveNodeClass(NodeType);
	if (!NodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown node_type '%s'. Supported: SequencePlayer, BlendSpacePlayer, TwoWayBlend, BlendListByBool, LayeredBoneBlend, MotionMatching"),
			*NodeType));
	}

	// Resolve the target graph
	FString GraphError;
	UEdGraph* TargetGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
	if (!TargetGraph) return FMonolithActionResult::Error(GraphError);

	// Create the template node on the transient package (will be duplicated by PerformAction)
	UAnimGraphNode_Base* Template = Cast<UAnimGraphNode_Base>(NewObject<UObject>(GetTransientPackage(), NodeClass));
	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create node template for type '%s'"), *NodeType));
	}

	// Set animation asset before spawning (gets duplicated with the node)
	if (!AnimAsset.IsEmpty())
	{
		UAnimGraphNode_AssetPlayerBase* AssetPlayer = Cast<UAnimGraphNode_AssetPlayerBase>(Template);
		if (AssetPlayer)
		{
			UAnimationAsset* Asset = FMonolithAssetUtils::LoadAssetByPath<UAnimationAsset>(AnimAsset);
			if (!Asset)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimAsset));
			}
			AssetPlayer->SetAnimationAsset(Asset);
		}
		else
		{
			// Non-asset-player node doesn't support anim_asset — just warn via log, don't fail
			UE_LOG(LogTemp, Warning, TEXT("Monolith: Node type '%s' does not support anim_asset parameter — ignored"), *NodeType);
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Anim Graph Node")));
	TargetGraph->Modify();

	// Spawn via FEdGraphSchemaAction_K2NewNode — same path as the editor
	FEdGraphSchemaAction_K2NewNode Action;
	Action.NodeTemplate = Template;
	UEdGraphNode* SpawnedNode = Action.PerformAction(TargetGraph, /*FromPin=*/nullptr, FVector2f(PosX, PosY), /*bSelectNewNode=*/false);

	GEditor->EndTransaction();

	if (!SpawnedNode)
	{
		return FMonolithActionResult::Error(TEXT("PerformAction failed — node was not spawned. Check that the target graph supports this node type."));
	}

	// Do NOT compile here — caller should batch node adds then wire, then compile once.
	// Just mark dirty.
	ABP->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("node_name"), SpawnedNode->GetName());
	Root->SetStringField(TEXT("node_class"), SpawnedNode->GetClass()->GetName());
	Root->SetStringField(TEXT("node_guid"), SpawnedNode->NodeGuid.ToString());
	Root->SetNumberField(TEXT("position_x"), SpawnedNode->NodePosX);
	Root->SetNumberField(TEXT("position_y"), SpawnedNode->NodePosY);
	Root->SetArrayField(TEXT("pins"), BuildPinList(SpawnedNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: connect_anim_graph_pins
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleConnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath  = Params->GetStringField(TEXT("asset_path"));
	FString SourceNode = Params->GetStringField(TEXT("source_node"));
	FString SourcePin  = Params->GetStringField(TEXT("source_pin"));
	FString TargetNode = Params->GetStringField(TEXT("target_node"));
	FString TargetPin  = Params->GetStringField(TEXT("target_pin"));
	FString GraphName  = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("");
	FString StateName  = Params->HasField(TEXT("state_name")) ? Params->GetStringField(TEXT("state_name")) : TEXT("");

	bool bCompile = true;
	if (Params->HasField(TEXT("compile")))
	{
		bCompile = Params->GetBoolField(TEXT("compile"));
	}

	if (SourceNode.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: source_node"));
	if (SourcePin.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: source_pin"));
	if (TargetNode.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: target_node"));
	if (TargetPin.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: target_pin"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Optionally resolve to a specific graph for scoping the search
	UEdGraph* ScopeGraph = nullptr;
	if (!StateName.IsEmpty() || (!GraphName.IsEmpty() && !GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase)))
	{
		FString GraphError;
		ScopeGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
		// If scope resolution fails, we still search globally as fallback
	}

	// Find source and target nodes
	UEdGraphNode* SrcNode = FindNodeByName(ABP, SourceNode, ScopeGraph);
	if (!SrcNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source node '%s' not found in ABP"), *SourceNode));
	}

	UEdGraphNode* DstNode = FindNodeByName(ABP, TargetNode, ScopeGraph);
	if (!DstNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target node '%s' not found in ABP"), *TargetNode));
	}

	// Find output pin on source
	UEdGraphPin* OutPin = SrcNode->FindPin(FName(*SourcePin), EGPD_Output);
	if (!OutPin)
	{
		// List available output pins for debugging
		FString AvailPins;
		for (UEdGraphPin* P : SrcNode->Pins)
		{
			if (P && P->Direction == EGPD_Output)
			{
				if (!AvailPins.IsEmpty()) AvailPins += TEXT(", ");
				AvailPins += P->PinName.ToString();
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Output pin '%s' not found on node '%s'. Available output pins: [%s]"),
			*SourcePin, *SourceNode, *AvailPins));
	}

	// Find input pin on target
	UEdGraphPin* InPin = DstNode->FindPin(FName(*TargetPin), EGPD_Input);
	if (!InPin)
	{
		FString AvailPins;
		for (UEdGraphPin* P : DstNode->Pins)
		{
			if (P && P->Direction == EGPD_Input)
			{
				if (!AvailPins.IsEmpty()) AvailPins += TEXT(", ");
				AvailPins += P->PinName.ToString();
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input pin '%s' not found on node '%s'. Available input pins: [%s]"),
			*TargetPin, *TargetNode, *AvailPins));
	}

	// Verify both nodes are in the same graph
	if (SrcNode->GetGraph() != DstNode->GetGraph())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Source node '%s' and target node '%s' are in different graphs — connections must be within the same graph"),
			*SourceNode, *TargetNode));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Connect Anim Graph Pins")));
	SrcNode->GetGraph()->Modify();

	// Use the graph's own schema for the connection (UAnimationGraphSchema or UAnimationStateGraphSchema)
	const UEdGraphSchema* Schema = SrcNode->GetGraph()->GetSchema();
	const bool bConnected = Schema->TryCreateConnection(OutPin, InPin);

	GEditor->EndTransaction();

	if (!bConnected)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("TryCreateConnection failed: '%s.%s' -> '%s.%s'. Pin types may be incompatible."),
			*SourceNode, *SourcePin, *TargetNode, *TargetPin));
	}

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
		FKismetEditorUtilities::CompileBlueprint(ABP);
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_node"), SourceNode);
	Root->SetStringField(TEXT("source_pin"), SourcePin);
	Root->SetStringField(TEXT("target_node"), TargetNode);
	Root->SetStringField(TEXT("target_pin"), TargetPin);
	Root->SetBoolField(TEXT("compiled"), bCompile);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_state_animation
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleSetStateAnimation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString MachineName  = Params->GetStringField(TEXT("machine_name"));
	FString StateName    = Params->GetStringField(TEXT("state_name"));
	FString AnimAssetPath = Params->GetStringField(TEXT("anim_asset_path"));

	bool bLoop = false;
	if (Params->HasField(TEXT("loop")))
	{
		bLoop = Params->GetBoolField(TEXT("loop"));
	}

	bool bClearExisting = true;
	if (Params->HasField(TEXT("clear_existing")))
	{
		bClearExisting = Params->GetBoolField(TEXT("clear_existing"));
	}

	if (MachineName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (StateName.IsEmpty())    return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));
	if (AnimAssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: anim_asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Find the state machine and state
	UAnimationStateMachineGraph* SMGraph = FindSMGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateNode* StateNode = FindStateByName(SMGraph, StateName);
	if (!StateNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *StateName, *MachineName));

	UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
	if (!StateGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' has no inner animation graph"), *StateName));

	UAnimGraphNode_StateResult* ResultNode = StateGraph->MyResultNode;
	if (!ResultNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' has no result node — state graph may be corrupt"), *StateName));

	// Load the animation asset
	UAnimationAsset* AnimAsset = FMonolithAssetUtils::LoadAssetByPath<UAnimationAsset>(AnimAssetPath);
	if (!AnimAsset) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimAssetPath));

	// Determine node class using the engine's own mapping
	UClass* NodeClass = GetNodeClassForAsset(AnimAsset->GetClass());
	if (!NodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No animation player node type for asset class '%s'. Supported: AnimSequence, BlendSpace."),
			*AnimAsset->GetClass()->GetName()));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set State Animation")));
	StateGraph->Modify();

	// Find the Result input pin
	UEdGraphPin* ResultInputPin = ResultNode->FindPin(TEXT("Result"), EGPD_Input);
	if (!ResultInputPin)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Could not find 'Result' input pin on state result node"));
	}

	// Optionally clear existing nodes wired to the result
	if (bClearExisting)
	{
		// Collect nodes currently connected to the result pin
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphPin* LinkedPin : ResultInputPin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				NodesToRemove.Add(LinkedPin->GetOwningNode());
			}
		}

		// Break all connections to the result pin
		ResultInputPin->BreakAllPinLinks();

		// Remove the previously-wired nodes (but not the result node itself)
		for (UEdGraphNode* OldNode : NodesToRemove)
		{
			if (OldNode && OldNode != ResultNode)
			{
				OldNode->BreakAllNodeLinks();
				StateGraph->RemoveNode(OldNode);
			}
		}
	}

	// Create template node on transient package
	UAnimGraphNode_AssetPlayerBase* Template = Cast<UAnimGraphNode_AssetPlayerBase>(
		NewObject<UObject>(GetTransientPackage(), NodeClass));
	if (!Template)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to create animation player node template"));
	}

	Template->SetAnimationAsset(AnimAsset);
	Template->CopySettingsFromAnimationAsset(AnimAsset);

	// Set loop flag if requested — need to access the FAnimNode struct via reflection
	if (bLoop)
	{
		// Try to set bLoopAnimation via the runtime node struct
		FStructProperty* NodeProp = Template->GetFNodeProperty();
		if (NodeProp)
		{
			void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(Template);
			FProperty* LoopProp = NodeProp->Struct->FindPropertyByName(FName(TEXT("bLoopAnimation")));
			if (LoopProp)
			{
				FBoolProperty* BoolProp = CastField<FBoolProperty>(LoopProp);
				if (BoolProp)
				{
					BoolProp->SetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(NodePtr), true);
				}
			}
		}
	}

	// Spawn into the state graph, positioned to the left of the result node
	float SpawnX = static_cast<float>(ResultNode->NodePosX - 300);
	float SpawnY = static_cast<float>(ResultNode->NodePosY);

	FEdGraphSchemaAction_K2NewNode Action;
	Action.NodeTemplate = Template;
	UEdGraphNode* SpawnedNode = Action.PerformAction(StateGraph, /*FromPin=*/nullptr, FVector2f(SpawnX, SpawnY), /*bSelectNewNode=*/false);

	if (!SpawnedNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("PerformAction failed — animation player node was not spawned"));
	}

	// Wire the pose output to the result input
	UEdGraphPin* PoseOutput = SpawnedNode->FindPin(TEXT("Pose"), EGPD_Output);
	if (!PoseOutput)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Spawned node has no 'Pose' output pin — cannot wire to state result"));
	}

	const UEdGraphSchema* Schema = StateGraph->GetSchema();
	const bool bWired = Schema->TryCreateConnection(PoseOutput, ResultInputPin);

	GEditor->EndTransaction();

	if (!bWired)
	{
		return FMonolithActionResult::Error(TEXT("TryCreateConnection failed wiring Pose -> Result. The node was spawned but not connected."));
	}

	// Compile
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("state_name"), StateName);
	Root->SetStringField(TEXT("anim_asset_path"), AnimAssetPath);
	Root->SetStringField(TEXT("node_name"), SpawnedNode->GetName());
	Root->SetStringField(TEXT("node_class"), SpawnedNode->GetClass()->GetName());
	Root->SetBoolField(TEXT("loop"), bLoop);
	Root->SetBoolField(TEXT("cleared_existing"), bClearExisting);
	Root->SetArrayField(TEXT("pins"), BuildPinList(SpawnedNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_linked_layer (Wave 12)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleSetLinkedLayer(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NodeName  = Params->GetStringField(TEXT("node_name"));
	FString LayerName = Params->GetStringField(TEXT("layer_name"));

	bool bCompile = true;
	if (Params->HasField(TEXT("compile")))
	{
		bCompile = Params->GetBoolField(TEXT("compile"));
	}

	if (NodeName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: node_name"));
	if (LayerName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: layer_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UEdGraphNode* Node = FindNodeByName(ABP, NodeName);
	if (!Node) return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found in ABP"), *NodeName));

	UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(Node);
	if (!LayerNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' is not a LinkedAnimLayer node (actual class: %s)"),
			*NodeName, *Node->GetClass()->GetName()));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Linked Anim Layer")));
	LayerNode->Modify();

	// Set the Layer FName directly on the inner FAnimNode_LinkedAnimLayer struct.
	// We avoid calling SetLayerName/UpdateGuidForLayer/GetLayerName since those
	// are non-exported (MinimalAPI class) and would cause LNK2019 link errors.
	LayerNode->Node.Layer = FName(*LayerName);

	// Look up the interface GUID from the ABP's implemented interfaces.
	// The LinkedAnimLayer node needs InterfaceGuid to match the layer function's graph GUID.
	FGuid FoundGuid;
	bool bFoundGuid = false;
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == FName(*LayerName))
		{
			FoundGuid = Graph->GraphGuid;
			bFoundGuid = true;
			break;
		}
	}

	// Also search the interface's graphs if this ABP implements an ALI
	if (!bFoundGuid)
	{
		for (const FBPInterfaceDescription& InterfaceDesc : ABP->ImplementedInterfaces)
		{
			if (!InterfaceDesc.Interface) continue;
			UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceDesc.Interface->ClassGeneratedBy);
			if (!InterfaceBP) continue;
			for (UEdGraph* Graph : InterfaceBP->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FName(*LayerName))
				{
					FoundGuid = Graph->GraphGuid;
					bFoundGuid = true;
					break;
				}
			}
			if (bFoundGuid) break;
		}
	}

	if (bFoundGuid)
	{
		LayerNode->InterfaceGuid = FoundGuid;
	}

	// Set the Interface class on the runtime node (so it knows which ALI to use)
	// Find the ALI that contains this layer function
	for (const FBPInterfaceDescription& InterfaceDesc : ABP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface) continue;
		UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceDesc.Interface->ClassGeneratedBy);
		if (!InterfaceBP) continue;
		for (UEdGraph* Graph : InterfaceBP->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*LayerName))
			{
				LayerNode->Node.Interface = TSubclassOf<UAnimLayerInterface>(InterfaceDesc.Interface);
				break;
			}
		}
	}

	// Reconstruct the node to regenerate pins for the new layer
	LayerNode->ReconstructNode();

	GEditor->EndTransaction();

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
		FKismetEditorUtilities::CompileBlueprint(ABP);
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_name"), NodeName);
	Root->SetStringField(TEXT("layer_name"), LayerName);
	Root->SetStringField(TEXT("actual_layer"), LayerNode->Node.Layer.ToString());
	Root->SetBoolField(TEXT("compiled"), bCompile);
	Root->SetArrayField(TEXT("pins"), BuildPinList(LayerNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_anim_node_property (Wave 12)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleSetAnimNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString NodeName     = Params->GetStringField(TEXT("node_name"));
	FString PropertyName = Params->GetStringField(TEXT("property_name"));
	FString Value        = Params->GetStringField(TEXT("value"));

	bool bCompile = false;
	if (Params->HasField(TEXT("compile")))
	{
		bCompile = Params->GetBoolField(TEXT("compile"));
	}

	if (NodeName.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: node_name"));
	if (PropertyName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: property_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	UEdGraphNode* Node = FindNodeByName(ABP, NodeName);
	if (!Node) return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found in ABP"), *NodeName));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
	if (!AnimNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' is not an anim graph node (actual class: %s)"),
			*NodeName, *Node->GetClass()->GetName()));
	}

	// Get the inner FAnimNode struct via reflection
	FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
	if (!NodeProp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' has no FAnimNode property (GetFNodeProperty returned null)"), *NodeName));
	}

	void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);
	UScriptStruct* NodeStruct = NodeProp->Struct;

	// Find the property by name (search the full hierarchy)
	FProperty* TargetProp = nullptr;
	for (UScriptStruct* Current = NodeStruct; Current; Current = Cast<UScriptStruct>(Current->GetSuperStruct()))
	{
		TargetProp = Current->FindPropertyByName(FName(*PropertyName));
		if (TargetProp) break;
	}

	if (!TargetProp)
	{
		// List available properties for debugging
		FString AvailProps;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			if (Count++ > 40) { AvailProps += TEXT(", ..."); break; }
			if (!AvailProps.IsEmpty()) AvailProps += TEXT(", ");
			AvailProps += It->GetName();
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s. Available: [%s]"),
			*PropertyName, *NodeStruct->GetName(), *AvailProps));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Anim Node Property")));
	AnimNode->Modify();

	FString ResultType;
	FString ResultValue;
	bool bSuccess = false;

	// Handle FBoneReference — set BoneName sub-field
	FStructProperty* StructProp = CastField<FStructProperty>(TargetProp);
	if (StructProp && StructProp->Struct->GetFName() == FName(TEXT("BoneReference")))
	{
		void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(NodePtr);
		FProperty* BoneNameProp = StructProp->Struct->FindPropertyByName(FName(TEXT("BoneName")));
		if (BoneNameProp)
		{
			FNameProperty* NameProp = CastField<FNameProperty>(BoneNameProp);
			if (NameProp)
			{
				NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(StructPtr), FName(*Value));
				bSuccess = true;
				ResultType = TEXT("FBoneReference");
				ResultValue = Value;
			}
		}
		if (!bSuccess)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(TEXT("Failed to set BoneName on FBoneReference struct"));
		}
	}
	// Handle bool
	else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(TargetProp))
	{
		bool bVal = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1");
		BoolProp->SetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(NodePtr), bVal);
		bSuccess = true;
		ResultType = TEXT("bool");
		ResultValue = bVal ? TEXT("true") : TEXT("false");
	}
	// Handle int
	else if (FIntProperty* IntProp = CastField<FIntProperty>(TargetProp))
	{
		int32 IntVal = FCString::Atoi(*Value);
		IntProp->SetPropertyValue(IntProp->ContainerPtrToValuePtr<void>(NodePtr), IntVal);
		bSuccess = true;
		ResultType = TEXT("int32");
		ResultValue = FString::FromInt(IntVal);
	}
	// Handle float
	else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(TargetProp))
	{
		float FloatVal = FCString::Atof(*Value);
		FloatProp->SetPropertyValue(FloatProp->ContainerPtrToValuePtr<void>(NodePtr), FloatVal);
		bSuccess = true;
		ResultType = TEXT("float");
		ResultValue = FString::SanitizeFloat(FloatVal);
	}
	// Handle double
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(TargetProp))
	{
		double DoubleVal = FCString::Atod(*Value);
		DoubleProp->SetPropertyValue(DoubleProp->ContainerPtrToValuePtr<void>(NodePtr), DoubleVal);
		bSuccess = true;
		ResultType = TEXT("double");
		ResultValue = FString::Printf(TEXT("%f"), DoubleVal);
	}
	// Handle FName
	else if (FNameProperty* NameProp = CastField<FNameProperty>(TargetProp))
	{
		NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(NodePtr), FName(*Value));
		bSuccess = true;
		ResultType = TEXT("FName");
		ResultValue = Value;
	}
	// Handle FString
	else if (FStrProperty* StrProp = CastField<FStrProperty>(TargetProp))
	{
		StrProp->SetPropertyValue(StrProp->ContainerPtrToValuePtr<void>(NodePtr), Value);
		bSuccess = true;
		ResultType = TEXT("FString");
		ResultValue = Value;
	}
	// Handle byte/enum (TEnumAsByte or raw uint8)
	else if (FByteProperty* ByteProp = CastField<FByteProperty>(TargetProp))
	{
		if (ByteProp->Enum)
		{
			// Try exact name first, then display name
			int64 EnumVal = ByteProp->Enum->GetValueByNameString(Value);
			if (EnumVal == INDEX_NONE)
			{
				for (int32 i = 0; i < ByteProp->Enum->NumEnums() - 1; ++i)
				{
					if (ByteProp->Enum->GetDisplayNameTextByIndex(i).ToString() == Value)
					{
						EnumVal = ByteProp->Enum->GetValueByIndex(i);
						break;
					}
				}
			}
			if (EnumVal == INDEX_NONE)
			{
				GEditor->EndTransaction();
				FString EnumValues;
				for (int32 i = 0; i < ByteProp->Enum->NumEnums() - 1; ++i)
				{
					if (!EnumValues.IsEmpty()) EnumValues += TEXT(", ");
					EnumValues += FString::Printf(TEXT("'%s' (%s)"),
						*ByteProp->Enum->GetNameStringByIndex(i),
						*ByteProp->Enum->GetDisplayNameTextByIndex(i).ToString());
				}
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Enum value '%s' not found in %s. Available: [%s]"),
					*Value, *ByteProp->Enum->GetName(), *EnumValues));
			}
			ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(NodePtr), static_cast<uint8>(EnumVal));
			bSuccess = true;
			ResultType = FString::Printf(TEXT("enum:%s"), *ByteProp->Enum->GetName());
			ResultValue = ByteProp->Enum->GetNameStringByIndex(ByteProp->Enum->GetIndexByValue(EnumVal));
		}
		else
		{
			uint8 ByteVal = static_cast<uint8>(FCString::Atoi(*Value));
			ByteProp->SetPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(NodePtr), ByteVal);
			bSuccess = true;
			ResultType = TEXT("uint8");
			ResultValue = FString::FromInt(ByteVal);
		}
	}
	// Handle FEnumProperty (UE5 style enum)
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(TargetProp))
	{
		UEnum* Enum = EnumProp->GetEnum();
		if (Enum)
		{
			int64 EnumVal = Enum->GetValueByNameString(Value);
			if (EnumVal == INDEX_NONE)
			{
				for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
				{
					if (Enum->GetDisplayNameTextByIndex(i).ToString() == Value)
					{
						EnumVal = Enum->GetValueByIndex(i);
						break;
					}
				}
			}
			if (EnumVal == INDEX_NONE)
			{
				GEditor->EndTransaction();
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Enum value '%s' not found in %s"), *Value, *Enum->GetName()));
			}
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			void* PropAddr = EnumProp->ContainerPtrToValuePtr<void>(NodePtr);
			UnderlyingProp->SetIntPropertyValue(PropAddr, EnumVal);
			bSuccess = true;
			ResultType = FString::Printf(TEXT("enum:%s"), *Enum->GetName());
			ResultValue = Enum->GetNameStringByIndex(Enum->GetIndexByValue(EnumVal));
		}
	}

	GEditor->EndTransaction();

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unsupported property type '%s' for property '%s'. Supported: bool, int, float, double, FName, FString, enum, FBoneReference."),
			*TargetProp->GetClass()->GetName(), *PropertyName));
	}

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
		FKismetEditorUtilities::CompileBlueprint(ABP);
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_name"), NodeName);
	Root->SetStringField(TEXT("property_name"), PropertyName);
	Root->SetStringField(TEXT("property_type"), ResultType);
	Root->SetStringField(TEXT("value_set"), ResultValue);
	Root->SetBoolField(TEXT("compiled"), bCompile);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_property_binding (Wave 13) — Property Access binding via reflection
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleSetPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath    = Params->GetStringField(TEXT("asset_path"));
	FString NodeName     = Params->GetStringField(TEXT("node_name"));
	FString BindingName  = Params->GetStringField(TEXT("binding_name"));
	FString PropertyPath = Params->GetStringField(TEXT("property_path"));

	bool bCompile = true;
	if (Params->HasField(TEXT("compile")))
	{
		bCompile = Params->GetBoolField(TEXT("compile"));
	}

	if (NodeName.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing: node_name"));
	if (BindingName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing: binding_name"));
	if (PropertyPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing: property_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("ABP not found: %s"), *AssetPath));

	UEdGraphNode* Node = FindNodeByName(ABP, NodeName);
	if (!Node) return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeName));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
	if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(TEXT("'%s' is not an anim node"), *NodeName));

	// Get the Binding sub-object via reflection (avoids needing Private header for UAnimGraphNodeBinding)
	FObjectProperty* BindingProp = CastField<FObjectProperty>(
		AnimNode->GetClass()->FindPropertyByName(FName(TEXT("Binding"))));
	UObject* BindingObj = BindingProp
		? BindingProp->GetObjectPropertyValue(BindingProp->ContainerPtrToValuePtr<void>(AnimNode))
		: nullptr;
	if (!BindingObj)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' has no Binding sub-object (UPROPERTY 'Binding' not found or null)"), *NodeName));
	}

	// Access PropertyBindings TMap via reflection (no Private header include needed)
	FMapProperty* MapProp = CastField<FMapProperty>(
		BindingObj->GetClass()->FindPropertyByName(FName(TEXT("PropertyBindings"))));
	if (!MapProp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Binding object class '%s' has no 'PropertyBindings' map property"),
			*BindingObj->GetClass()->GetName()));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Property Access Binding")));
	AnimNode->Modify();
	BindingObj->Modify();

	// Get map helper
	FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(BindingObj));

	// Check if this binding already exists — if so, we'll update it
	int32 TargetIndex = INDEX_NONE;
	FNameProperty* KeyNameProp = CastField<FNameProperty>(MapProp->KeyProp);
	for (int32 i = 0; i < MapHelper.Num(); ++i)
	{
		if (MapHelper.IsValidIndex(i))
		{
			FName ExistingKey;
			KeyNameProp->GetValue_InContainer(MapHelper.GetKeyPtr(i), &ExistingKey);
			if (ExistingKey == FName(*BindingName))
			{
				TargetIndex = i;
				break;
			}
		}
	}

	if (TargetIndex == INDEX_NONE)
	{
		// Add new entry
		TargetIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
		MapHelper.Rehash();
		// Set key
		KeyNameProp->SetPropertyValue(MapHelper.GetKeyPtr(TargetIndex), FName(*BindingName));
	}

	// Now configure the value (FAnimGraphNodePropertyBinding struct) via reflection
	FStructProperty* ValueStructProp = CastField<FStructProperty>(MapProp->ValueProp);
	if (!ValueStructProp)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Map value is not a struct — unexpected"));
	}

	void* ValuePtr = MapHelper.GetValuePtr(TargetIndex);
	UScriptStruct* BindingStruct = ValueStructProp->Struct;

	// Set PropertyName = binding_name
	{
		FNameProperty* Prop = CastField<FNameProperty>(BindingStruct->FindPropertyByName(FName(TEXT("PropertyName"))));
		if (Prop) Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(ValuePtr), FName(*BindingName));
	}

	// Set bIsBound = true
	{
		FBoolProperty* Prop = CastField<FBoolProperty>(BindingStruct->FindPropertyByName(FName(TEXT("bIsBound"))));
		if (Prop) Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(ValuePtr), true);
	}

	// Set Type = Property (enum value 1)
	{
		FByteProperty* Prop = CastField<FByteProperty>(BindingStruct->FindPropertyByName(FName(TEXT("Type"))));
		if (Prop)
		{
			Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(ValuePtr), 1); // EAnimGraphNodePropertyBindingType::Property
		}
		else
		{
			// Might be an enum property in newer UE
			FEnumProperty* EnumProp = CastField<FEnumProperty>(BindingStruct->FindPropertyByName(FName(TEXT("Type"))));
			if (EnumProp)
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				if (UnderlyingProp)
				{
					UnderlyingProp->SetIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(ValuePtr), (int64)1);
				}
			}
		}
	}

	// Set PathAsText = property_path
	{
		FTextProperty* Prop = CastField<FTextProperty>(BindingStruct->FindPropertyByName(FName(TEXT("PathAsText"))));
		if (Prop)
		{
			FText PathText = FText::FromString(PropertyPath);
			Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(ValuePtr), PathText);
		}
	}

	// Set PropertyPath = [property_path] (TArray<FString>)
	{
		FArrayProperty* ArrProp = CastField<FArrayProperty>(BindingStruct->FindPropertyByName(FName(TEXT("PropertyPath"))));
		if (ArrProp)
		{
			FScriptArrayHelper ArrHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(ValuePtr));
			ArrHelper.EmptyValues(); // Clear existing
			ArrHelper.AddValue();
			FStrProperty* StrProp = CastField<FStrProperty>(ArrProp->Inner);
			if (StrProp)
			{
				StrProp->SetPropertyValue(ArrHelper.GetRawPtr(0), PropertyPath);
			}
		}
	}

	// Reconstruct node to apply the binding visually
	AnimNode->ReconstructNode();

	GEditor->EndTransaction();

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
		FKismetEditorUtilities::CompileBlueprint(ABP);
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_name"), NodeName);
	Root->SetStringField(TEXT("binding_name"), BindingName);
	Root->SetStringField(TEXT("property_path"), PropertyPath);
	Root->SetBoolField(TEXT("compiled"), bCompile);
	return FMonolithActionResult::Success(Root);
}
