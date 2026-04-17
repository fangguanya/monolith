#include "MonolithAbpDumpActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateConduitNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================
//  Registration
// ============================================================

void FMonolithAbpDumpActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("dump_abp_t3d"),
		TEXT("Recursively dump an entire Animation Blueprint to T3D text — the structural equivalent of "
			"opening every graph and subgraph in the editor and pressing Ctrl+A+Ctrl+C. Walks UbergraphPages, "
			"FunctionGraphs (including main AnimGraph), MacroGraphs, DelegateSignatureGraphs, and recursively "
			"descends into state machine inner graphs (states, transitions, conduits) + nested state machines. "
			"Returns a map { graph_path: { node_count, text_bytes, t3d_text } }. This is the silver-bullet "
			"read action: raw T3D preserves ALL node state including FAnimNode_ struct fields, property "
			"bindings, LinkedAnimLayer interface/function references, SkelControl bone targets, LayeredBoneBlend "
			"layer setup, etc. — things normal JSON serialization cannot see."),
		FMonolithActionHandler::CreateStatic(&HandleDumpAbpT3D),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("include_inner_graphs"), TEXT("bool"), TEXT("Recurse into state machine inner graphs (default: true)"), TEXT("true"))
			.Optional(TEXT("max_bytes_per_graph"), TEXT("integer"), TEXT("Soft cap per-graph T3D text length (0 = no cap, default: 0). Graphs exceeding the cap still count but return a truncation flag."))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("list_abp_graphs"),
		TEXT("Enumerate every graph reachable from an Animation Blueprint as a flat list of "
			"{ path, kind, node_count } entries. Use this first to scope an export, then call "
			"blueprint.export_nodes_t3d on specific graphs, or animation.dump_abp_t3d for the full tree."),
		FMonolithActionHandler::CreateStatic(&HandleListAbpGraphs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("include_inner_graphs"), TEXT("bool"), TEXT("Recurse into state machine inner graphs (default: true)"), TEXT("true"))
			.Build());
}

// ============================================================
//  Graph collection helpers
// ============================================================

namespace
{
	struct FCollectedGraph
	{
		UEdGraph* Graph = nullptr;
		FString Path;    // e.g. "AnimGraph/LocomotionSM/Idle" or "AnimGraph/LocomotionSM/Idle->Walk"
		FString Kind;    // "event_graph" | "function" | "macro" | "delegate_signature" | "state_machine" | "state" | "transition" | "conduit"
	};

	/** Recurse into a state machine graph and collect inner state / transition / conduit graphs. */
	void CollectStateMachineGraphs(
		UAnimationStateMachineGraph* SMGraph,
		const FString& ParentPath,
		TArray<FCollectedGraph>& OutGraphs,
		TSet<UEdGraph*>& Visited)
	{
		if (!SMGraph || Visited.Contains(SMGraph)) return;
		Visited.Add(SMGraph);

		// The state machine graph itself
		OutGraphs.Add({SMGraph, ParentPath, TEXT("state_machine")});

		for (UEdGraphNode* Node : SMGraph->Nodes)
		{
			if (!Node) continue;

			if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
			{
				if (UEdGraph* StateInner = StateNode->BoundGraph)
				{
					if (Visited.Contains(StateInner)) continue;
					Visited.Add(StateInner);

					const FString StatePath = FString::Printf(TEXT("%s/%s"), *ParentPath, *StateNode->GetStateName());
					OutGraphs.Add({StateInner, StatePath, TEXT("state")});

					// A state inner graph can contain nested state machines
					for (UEdGraphNode* InnerNode : StateInner->Nodes)
					{
						if (UAnimGraphNode_StateMachine* NestedSM = Cast<UAnimGraphNode_StateMachine>(InnerNode))
						{
							if (UAnimationStateMachineGraph* NestedSMGraph = Cast<UAnimationStateMachineGraph>(NestedSM->EditorStateMachineGraph))
							{
								const FString NestedPath = FString::Printf(TEXT("%s/%s"), *StatePath, *NestedSM->GetNodeTitle(ENodeTitleType::ListView).ToString());
								CollectStateMachineGraphs(NestedSMGraph, NestedPath, OutGraphs, Visited);
							}
						}
					}
				}
			}
			else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
			{
				if (UEdGraph* ConduitInner = ConduitNode->BoundGraph)
				{
					if (Visited.Contains(ConduitInner)) continue;
					Visited.Add(ConduitInner);
					const FString ConduitPath = FString::Printf(TEXT("%s/%s"), *ParentPath, *ConduitNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
					OutGraphs.Add({ConduitInner, ConduitPath, TEXT("conduit")});
				}
			}
			else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
			{
				if (UEdGraph* TransInner = TransNode->BoundGraph)
				{
					if (Visited.Contains(TransInner)) continue;
					Visited.Add(TransInner);

					FString FromName = TEXT("?");
					FString ToName = TEXT("?");
					if (UAnimStateNodeBase* Prev = TransNode->GetPreviousState()) FromName = Prev->GetStateName();
					if (UAnimStateNodeBase* Next = TransNode->GetNextState()) ToName = Next->GetStateName();

					const FString TransPath = FString::Printf(TEXT("%s/%s->%s"), *ParentPath, *FromName, *ToName);
					OutGraphs.Add({TransInner, TransPath, TEXT("transition")});
				}
			}
		}
	}

	/** Collect all top-level graphs + inner state machine subgraphs from an ABP. */
	void CollectAllAbpGraphs(
		UAnimBlueprint* ABP,
		bool bIncludeInnerGraphs,
		TArray<FCollectedGraph>& OutGraphs)
	{
		TSet<UEdGraph*> Visited;

		auto AddArray = [&](const TArray<TObjectPtr<UEdGraph>>& Arr, const FString& Kind)
		{
			for (const TObjectPtr<UEdGraph>& Graph : Arr)
			{
				if (!Graph || Visited.Contains(Graph)) continue;
				Visited.Add(Graph);
				OutGraphs.Add({Graph, Graph->GetName(), Kind});

				if (!bIncludeInnerGraphs) continue;

				// Scan for state machine nodes inside this graph and recurse
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
					{
						if (UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph))
						{
							const FString SMPath = FString::Printf(TEXT("%s/%s"), *Graph->GetName(), *SMNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
							CollectStateMachineGraphs(SMGraph, SMPath, OutGraphs, Visited);
						}
					}
				}
			}
		};

		AddArray(ABP->UbergraphPages, TEXT("event_graph"));
		AddArray(ABP->FunctionGraphs, TEXT("function"));
		AddArray(ABP->MacroGraphs, TEXT("macro"));
		AddArray(ABP->DelegateSignatureGraphs, TEXT("delegate_signature"));
	}
}

// ============================================================
//  dump_abp_t3d
// ============================================================

FMonolithActionResult FMonolithAbpDumpActions::HandleDumpAbpT3D(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));
	}

	bool bIncludeInner = true;
	if (Params->HasField(TEXT("include_inner_graphs")))
	{
		bIncludeInner = Params->GetBoolField(TEXT("include_inner_graphs"));
	}

	int32 MaxBytesPerGraph = 0;
	{
		double TempVal = 0;
		if (Params->TryGetNumberField(TEXT("max_bytes_per_graph"), TempVal))
		{
			MaxBytesPerGraph = static_cast<int32>(TempVal);
		}
	}

	TArray<FCollectedGraph> Collected;
	CollectAllAbpGraphs(ABP, bIncludeInner, Collected);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("parent_class"), ABP->ParentClass ? ABP->ParentClass->GetName() : FString());
	Root->SetNumberField(TEXT("graph_count"), Collected.Num());

	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	int64 TotalBytes = 0;
	int32 TruncatedCount = 0;

	for (const FCollectedGraph& CG : Collected)
	{
		if (!CG.Graph) continue;

		TSet<UObject*> NodesToExport;
		for (UEdGraphNode* N : CG.Graph->Nodes)
		{
			if (N) NodesToExport.Add(N);
		}

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("path"), CG.Path);
		GraphObj->SetStringField(TEXT("kind"), CG.Kind);
		GraphObj->SetStringField(TEXT("graph_name"), CG.Graph->GetName());
		GraphObj->SetStringField(TEXT("graph_class"), CG.Graph->GetClass()->GetName());
		GraphObj->SetNumberField(TEXT("node_count"), NodesToExport.Num());

		if (NodesToExport.Num() == 0)
		{
			GraphObj->SetStringField(TEXT("t3d_text"), FString());
			GraphObj->SetNumberField(TEXT("text_bytes"), 0);
			GraphsArr.Add(MakeShared<FJsonValueObject>(GraphObj));
			continue;
		}

		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

		const int32 OriginalLen = ExportedText.Len();
		bool bTruncated = false;
		if (MaxBytesPerGraph > 0 && OriginalLen > MaxBytesPerGraph)
		{
			ExportedText = ExportedText.Left(MaxBytesPerGraph);
			bTruncated = true;
			TruncatedCount++;
		}

		GraphObj->SetStringField(TEXT("t3d_text"), ExportedText);
		GraphObj->SetNumberField(TEXT("text_bytes"), OriginalLen);
		if (bTruncated)
		{
			GraphObj->SetBoolField(TEXT("truncated"), true);
		}

		TotalBytes += OriginalLen;
		GraphsArr.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	Root->SetArrayField(TEXT("graphs"), GraphsArr);
	Root->SetNumberField(TEXT("total_text_bytes"), static_cast<double>(TotalBytes));
	if (TruncatedCount > 0)
	{
		Root->SetNumberField(TEXT("truncated_graph_count"), TruncatedCount);
	}

	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  list_abp_graphs
// ============================================================

FMonolithActionResult FMonolithAbpDumpActions::HandleListAbpGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));
	}

	bool bIncludeInner = true;
	if (Params->HasField(TEXT("include_inner_graphs")))
	{
		bIncludeInner = Params->GetBoolField(TEXT("include_inner_graphs"));
	}

	TArray<FCollectedGraph> Collected;
	CollectAllAbpGraphs(ABP, bIncludeInner, Collected);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("graph_count"), Collected.Num());

	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	for (const FCollectedGraph& CG : Collected)
	{
		if (!CG.Graph) continue;
		TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
		GObj->SetStringField(TEXT("path"), CG.Path);
		GObj->SetStringField(TEXT("kind"), CG.Kind);
		GObj->SetStringField(TEXT("graph_name"), CG.Graph->GetName());
		GObj->SetStringField(TEXT("graph_class"), CG.Graph->GetClass()->GetName());
		GObj->SetNumberField(TEXT("node_count"), CG.Graph->Nodes.Num());
		GraphsArr.Add(MakeShared<FJsonValueObject>(GObj));
	}
	Root->SetArrayField(TEXT("graphs"), GraphsArr);

	return FMonolithActionResult::Success(Root);
}
