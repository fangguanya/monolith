#include "MonolithBlueprintActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/SceneComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"

// --- Registration ---

void FMonolithBlueprintActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("blueprint"), TEXT("list_graphs"),
		TEXT("List all graphs in a Blueprint asset"),
		FMonolithActionHandler::CreateStatic(&HandleListGraphs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_graph_data"),
		TEXT("Get full graph data with all nodes, pins, and connections. Optional node_class_filter to include only matching node classes."),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (defaults to first event graph)"))
			.Optional(TEXT("node_class_filter"), TEXT("string"), TEXT("Only include nodes whose class contains this substring"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_graph_summary"),
		TEXT("Get lightweight graph summary with node id/class/title and exec-only connections. Returns all graphs when graph_name is empty."),
		FMonolithActionHandler::CreateStatic(&HandleGetGraphSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name (returns all graphs when empty)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_variables"),
		TEXT("Get all variables defined in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_execution_flow"),
		TEXT("Get linearized execution flow from an entry point"),
		FMonolithActionHandler::CreateStatic(&HandleGetExecutionFlow),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("entry_point"), TEXT("string"), TEXT("Event or function entry point name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("search_nodes"),
		TEXT("Search for nodes in a Blueprint by title or function name"),
		FMonolithActionHandler::CreateStatic(&HandleSearchNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("query"), TEXT("string"), TEXT("Search string to match against node titles and function names"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_components"),
		TEXT("Get component hierarchy for a Blueprint — names, classes, parent-child tree, attach sockets"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponents),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_component_details"),
		TEXT("Get full property dump for a specific component in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponentDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_functions"),
		TEXT("Get all Blueprint-defined functions with inputs, outputs, metadata, and flags"),
		FMonolithActionHandler::CreateStatic(&HandleGetFunctions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_event_dispatchers"),
		TEXT("Get all event dispatchers (multicast delegates) defined in a Blueprint with their signature pins"),
		FMonolithActionHandler::CreateStatic(&HandleGetEventDispatchers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_parent_class"),
		TEXT("Get parent class info, Blueprint type, and class flags for a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetParentClass),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_interfaces"),
		TEXT("Get all interfaces implemented by a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetInterfaces),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_construction_script"),
		TEXT("Get all nodes in the Construction Script graph"),
		FMonolithActionHandler::CreateStatic(&HandleGetConstructionScript),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());
}

// --- Shared helper ---

UBlueprint* FMonolithBlueprintActions::LoadBlueprint(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath)
{
	return MonolithBlueprintInternal::LoadBlueprintFromParams(Params, OutAssetPath);
}

// --- list_graphs ---

FMonolithActionResult FMonolithBlueprintActions::HandleListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("class"), BP->GetClass()->GetName());

	if (BP->ParentClass)
	{
		Root->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->UbergraphPages, TEXT("event_graph"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->FunctionGraphs, TEXT("function"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->MacroGraphs, TEXT("macro"));
	MonolithBlueprintInternal::AddGraphArray(GraphsArr, BP->DelegateSignatureGraphs, TEXT("delegate_signature"));
	Root->SetArrayField(TEXT("graphs"), GraphsArr);

	return FMonolithActionResult::Success(Root);
}

// --- get_graph_data ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetGraphData(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString GraphName = Params->GetStringField(TEXT("graph_name"));
	FString ClassFilter = Params->GetStringField(TEXT("node_class_filter"));
	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), Graph->GetName());

	FString GraphType = TEXT("unknown");
	if (BP->UbergraphPages.Contains(Graph)) GraphType = TEXT("event_graph");
	else if (BP->FunctionGraphs.Contains(Graph)) GraphType = TEXT("function");
	else if (BP->MacroGraphs.Contains(Graph)) GraphType = TEXT("macro");
	else if (BP->DelegateSignatureGraphs.Contains(Graph)) GraphType = TEXT("delegate_signature");
	Root->SetStringField(TEXT("graph_type"), GraphType);

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		if (!ClassFilter.IsEmpty() && !Node->GetClass()->GetName().Contains(ClassFilter)) continue;
		NodesArr.Add(MakeShared<FJsonValueObject>(MonolithBlueprintInternal::SerializeNode(Node)));
	}
	Root->SetArrayField(TEXT("nodes"), NodesArr);

	return FMonolithActionResult::Success(Root);
}

// --- get_graph_summary ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetGraphSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	// Lambda: summarize a single graph into a node array
	auto SummarizeGraph = [](UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
			NObj->SetStringField(TEXT("id"), Node->GetName());
			NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			NObj->SetStringField(TEXT("title"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

			if (!Node->NodeComment.IsEmpty())
			{
				NObj->SetStringField(TEXT("comment"), Node->NodeComment);
			}

			// Collect exec-only output connections
			TArray<TSharedPtr<FJsonValue>> ExecTargets;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					ExecTargets.Add(MakeShared<FJsonValueString>(Linked->GetOwningNode()->GetName()));
				}
			}
			if (ExecTargets.Num() > 0)
			{
				NObj->SetArrayField(TEXT("exec_targets"), ExecTargets);
			}

			OutNodes.Add(MakeShared<FJsonValueObject>(NObj));
		}
	};

	FString GraphName = Params->GetStringField(TEXT("graph_name"));

	// When graph_name is provided, return single graph at root level (backward compat)
	if (!GraphName.IsEmpty())
	{
		UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, GraphName);
		if (!Graph)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("graph_name"), Graph->GetName());

		TArray<TSharedPtr<FJsonValue>> NodesArr;
		SummarizeGraph(Graph, NodesArr);
		Root->SetArrayField(TEXT("nodes"), NodesArr);
		Root->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		return FMonolithActionResult::Success(Root);
	}

	// When graph_name is empty, return all graphs
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> GraphsArr;
	int32 TotalNodeCount = 0;

	struct FGraphArrayInfo
	{
		const TArray<TObjectPtr<UEdGraph>>* Graphs;
		const TCHAR* Type;
	};

	TArray<FGraphArrayInfo> GraphArrays = {
		{ &BP->UbergraphPages, TEXT("event_graph") },
		{ &BP->FunctionGraphs, TEXT("function") },
		{ &BP->MacroGraphs, TEXT("macro") },
		{ &BP->DelegateSignatureGraphs, TEXT("delegate_signature") }
	};

	for (const FGraphArrayInfo& Info : GraphArrays)
	{
		for (const auto& Graph : *Info.Graphs)
		{
			if (!Graph) continue;

			TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
			GObj->SetStringField(TEXT("graph_name"), Graph->GetName());
			GObj->SetStringField(TEXT("graph_type"), Info.Type);

			TArray<TSharedPtr<FJsonValue>> NodesArr;
			SummarizeGraph(Graph, NodesArr);
			GObj->SetArrayField(TEXT("nodes"), NodesArr);
			GObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

			TotalNodeCount += Graph->Nodes.Num();
			GraphsArr.Add(MakeShared<FJsonValueObject>(GObj));
		}
	}

	Root->SetArrayField(TEXT("graphs"), GraphsArr);
	Root->SetNumberField(TEXT("graph_count"), GraphsArr.Num());
	Root->SetNumberField(TEXT("total_node_count"), TotalNodeCount);

	return FMonolithActionResult::Success(Root);
}

// --- get_variables ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> VarsArr;

	UClass* GenClass = BP->GeneratedClass;
	UObject* CDO = GenClass ? GenClass->GetDefaultObject(false) : nullptr;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VObj = MakeShared<FJsonObject>();
		VObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VObj->SetStringField(TEXT("type"),
			MonolithBlueprintInternal::ContainerPrefix(Var.VarType) +
			MonolithBlueprintInternal::PinTypeToString(Var.VarType));

		FString DefaultStr = Var.DefaultValue;
		if (DefaultStr.IsEmpty() && CDO && GenClass)
		{
			FProperty* Prop = GenClass->FindPropertyByName(Var.VarName);
			if (Prop)
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				Prop->ExportTextItem_Direct(DefaultStr, ValuePtr, nullptr, CDO, PPF_None);
			}
		}
		VObj->SetStringField(TEXT("default_value"), DefaultStr);
		VObj->SetStringField(TEXT("category"), Var.Category.ToString());

		VObj->SetBoolField(TEXT("instance_editable"),
			(Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VObj->SetBoolField(TEXT("blueprint_read_only"),
			(Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
		VObj->SetBoolField(TEXT("expose_on_spawn"),
			(Var.PropertyFlags & CPF_ExposeOnSpawn) != 0);
		VObj->SetBoolField(TEXT("replicated"),
			(Var.PropertyFlags & CPF_Net) != 0);
		VObj->SetBoolField(TEXT("transient"),
			(Var.PropertyFlags & CPF_Transient) != 0);

		VarsArr.Add(MakeShared<FJsonValueObject>(VObj));
	}

	Root->SetArrayField(TEXT("variables"), VarsArr);
	return FMonolithActionResult::Success(Root);
}

// --- get_execution_flow ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetExecutionFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString EntryPoint = Params->GetStringField(TEXT("entry_point"));
	if (EntryPoint.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: entry_point"));
	}

	UEdGraphNode* EntryNode = nullptr;
	UEdGraph* FoundGraph = nullptr;

	auto SearchGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs)
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			UEdGraphNode* Node = MonolithBlueprintInternal::FindEntryNode(Graph, EntryPoint);
			if (Node)
			{
				EntryNode = Node;
				FoundGraph = Graph;
				return;
			}
		}
	};

	SearchGraphs(BP->UbergraphPages);
	if (!EntryNode) SearchGraphs(BP->FunctionGraphs);
	if (!EntryNode) SearchGraphs(BP->MacroGraphs);

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Entry point not found: %s"), *EntryPoint));
	}

	TSet<UEdGraphNode*> Visited;
	TSharedPtr<FJsonObject> Flow = MonolithBlueprintInternal::TraceExecFlow(EntryNode, Visited);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("entry"), EntryPoint);
	Root->SetStringField(TEXT("graph"), FoundGraph->GetName());
	if (Flow)
	{
		Root->SetObjectField(TEXT("flow"), Flow);
	}

	return FMonolithActionResult::Success(Root);
}

// --- search_nodes ---

FMonolithActionResult FMonolithBlueprintActions::HandleSearchNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString Query = Params->GetStringField(TEXT("query"));
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: query"));
	}

	FString QueryLower = Query.ToLower();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Results;

	auto SearchGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs, const FString& Type)
	{
		for (const auto& Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				FString ClassName = Node->GetClass()->GetName();
				FString FuncName;

				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
				{
					FuncName = CallNode->FunctionReference.GetMemberName().ToString();
				}

				bool bMatch = Title.ToLower().Contains(QueryLower) ||
							  ClassName.ToLower().Contains(QueryLower) ||
							  FuncName.ToLower().Contains(QueryLower);

				if (bMatch)
				{
					TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
					RObj->SetStringField(TEXT("graph"), Graph->GetName());
					RObj->SetStringField(TEXT("graph_type"), Type);
					RObj->SetStringField(TEXT("node_id"), Node->GetName());
					RObj->SetStringField(TEXT("class"), ClassName);
					RObj->SetStringField(TEXT("title"), Title);
					if (!FuncName.IsEmpty())
					{
						RObj->SetStringField(TEXT("function"), FuncName);
					}
					Results.Add(MakeShared<FJsonValueObject>(RObj));
				}
			}
		}
	};

	SearchGraphs(BP->UbergraphPages, TEXT("event_graph"));
	SearchGraphs(BP->FunctionGraphs, TEXT("function"));
	SearchGraphs(BP->MacroGraphs, TEXT("macro"));
	SearchGraphs(BP->DelegateSignatureGraphs, TEXT("delegate_signature"));

	Root->SetStringField(TEXT("query"), Query);
	Root->SetNumberField(TEXT("match_count"), Results.Num());
	Root->SetArrayField(TEXT("results"), Results);

	return FMonolithActionResult::Success(Root);
}

// --- get_components ---

namespace
{
	// Forward declaration for recursive serialization
	TSharedPtr<FJsonObject> SerializeSCSNode(USCS_Node* Node);

	TSharedPtr<FJsonObject> SerializeSCSNode(USCS_Node* Node)
	{
		if (!Node) return nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

		FString VarName = Node->GetVariableName().ToString();
		Obj->SetStringField(TEXT("variable_name"), VarName);
		Obj->SetStringField(TEXT("name"), VarName);

		if (Node->ComponentClass)
		{
			Obj->SetStringField(TEXT("class"), Node->ComponentClass->GetName());
			Obj->SetBoolField(TEXT("is_scene_component"),
				Node->ComponentClass->IsChildOf(USceneComponent::StaticClass()));
		}
		else
		{
			Obj->SetStringField(TEXT("class"), TEXT("unknown"));
			Obj->SetBoolField(TEXT("is_scene_component"), false);
		}

		Obj->SetBoolField(TEXT("is_root"), Node->IsRootNode());

		FName AttachSocket = Node->AttachToName;
		if (!AttachSocket.IsNone())
		{
			Obj->SetStringField(TEXT("attach_to"), AttachSocket.ToString());
		}

		// Serialize children recursively
		TArray<TSharedPtr<FJsonValue>> ChildrenArr;
		for (USCS_Node* Child : Node->GetChildNodes())
		{
			TSharedPtr<FJsonObject> ChildObj = SerializeSCSNode(Child);
			if (ChildObj)
			{
				ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
		}
		Obj->SetArrayField(TEXT("children"), ChildrenArr);

		return Obj;
	}
} // anonymous namespace

FMonolithActionResult FMonolithBlueprintActions::HandleGetComponents(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> ComponentsArr;
	int32 ComponentCount = 0;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS)
	{
		// Walk root nodes — each represents a top-level component
		for (USCS_Node* RootNode : SCS->GetRootNodes())
		{
			if (!RootNode) continue;
			TSharedPtr<FJsonObject> NodeObj = SerializeSCSNode(RootNode);
			if (NodeObj)
			{
				ComponentsArr.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}

		// Count all nodes (flat) for the component_count field
		TArray<USCS_Node*> AllNodes = SCS->GetAllNodes();
		ComponentCount = AllNodes.Num();
	}

	// Also surface inherited native components from the parent class chain
	TArray<TSharedPtr<FJsonValue>> NativeComponentsArr;
	if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		AActor* CDO = Cast<AActor>(BP->ParentClass->GetDefaultObject(false));
		if (CDO)
		{
			TArray<UActorComponent*> NativeComps;
			CDO->GetComponents(NativeComps);
			for (UActorComponent* Comp : NativeComps)
			{
				if (!Comp) continue;
				TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
				NObj->SetStringField(TEXT("name"), Comp->GetName());
				NObj->SetStringField(TEXT("variable_name"), Comp->GetName());
				NObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
				NObj->SetBoolField(TEXT("is_scene_component"), Comp->IsA(USceneComponent::StaticClass()));
				NObj->SetBoolField(TEXT("is_native"), true);

				if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
				{
					if (USceneComponent* AttachParent = SceneComp->GetAttachParent())
					{
						NObj->SetStringField(TEXT("parent"), AttachParent->GetName());
					}
					NObj->SetBoolField(TEXT("is_root"), SceneComp->GetAttachParent() == nullptr);
				}
				NativeComponentsArr.Add(MakeShared<FJsonValueObject>(NObj));
			}
		}
	}

	Root->SetArrayField(TEXT("components"), ComponentsArr);
	Root->SetNumberField(TEXT("component_count"), ComponentCount);
	if (NativeComponentsArr.Num() > 0)
	{
		Root->SetArrayField(TEXT("inherited_native_components"), NativeComponentsArr);
	}

	return FMonolithActionResult::Success(Root);
}

// --- get_component_details ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetComponentDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ComponentName = Params->GetStringField(TEXT("component_name"));
	if (ComponentName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: component_name"));
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	UActorComponent* Template = Node->ComponentTemplate;
	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component template is null for: %s"), *ComponentName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component_name"), ComponentName);
	Root->SetStringField(TEXT("class"), Template->GetClass()->GetName());
	Root->SetBoolField(TEXT("is_scene_component"), Template->IsA(USceneComponent::StaticClass()));

	// For USceneComponent, include transform explicitly
	if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Template))
	{
		TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();

		FVector Loc = SceneTemplate->GetRelativeLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		TransformObj->SetObjectField(TEXT("relative_location"), LocObj);

		FRotator Rot = SceneTemplate->GetRelativeRotation();
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
		TransformObj->SetObjectField(TEXT("relative_rotation"), RotObj);

		FVector Scale = SceneTemplate->GetRelativeScale3D();
		TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
		ScaleObj->SetNumberField(TEXT("x"), Scale.X);
		ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
		ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
		TransformObj->SetObjectField(TEXT("relative_scale"), ScaleObj);

		Root->SetObjectField(TEXT("transform"), TransformObj);
	}

	// Iterate properties via reflection — include CPF_Edit or CPF_BlueprintVisible
	TArray<TSharedPtr<FJsonValue>> PropsArr;
	UClass* ComponentClass = Template->GetClass();

	// Walk class hierarchy, stop at UActorComponent (don't dump UObject internals)
	for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop) continue;

		// Only expose editor-visible or Blueprint-accessible properties
		if (!(Prop->PropertyFlags & (CPF_Edit | CPF_BlueprintVisible))) continue;

		// Skip properties that belong to UObject itself (too low-level / internal)
		if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Template, PPF_None);

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("value"), ValueStr);

		// Include the category metadata if present
		FString Category = Prop->GetMetaData(TEXT("Category"));
		if (!Category.IsEmpty())
		{
			PropObj->SetStringField(TEXT("category"), Category);
		}

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	Root->SetArrayField(TEXT("properties"), PropsArr);
	Root->SetNumberField(TEXT("property_count"), PropsArr.Num());

	return FMonolithActionResult::Success(Root);
}

// --- get_functions ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> FuncsArr;

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
		FObj->SetStringField(TEXT("name"), Graph->GetName());

		// Find entry node for metadata and input pins
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (EntryNode && ResultNode) break;
		}

		// Flags from entry node metadata
		if (EntryNode)
		{
			const uint32 ExtraFlags = EntryNode->GetFunctionFlags();
			FObj->SetBoolField(TEXT("is_pure"), (ExtraFlags & FUNC_BlueprintPure) != 0);
			FObj->SetBoolField(TEXT("is_const"), (ExtraFlags & FUNC_Const) != 0);
			FObj->SetBoolField(TEXT("is_static"), (ExtraFlags & FUNC_Static) != 0);
			FObj->SetBoolField(TEXT("call_in_editor"), EntryNode->MetaData.bCallInEditor);
			FObj->SetStringField(TEXT("category"), EntryNode->MetaData.Category.ToString());
			FObj->SetStringField(TEXT("description"), EntryNode->MetaData.ToolTip.ToString());
			FObj->SetStringField(TEXT("access_specifier"),
				(ExtraFlags & FUNC_Protected) ? TEXT("Protected") :
				(ExtraFlags & FUNC_Private)   ? TEXT("Private")   : TEXT("Public"));

			// Inputs: output pins on the entry node (excluding exec)
			TArray<TSharedPtr<FJsonValue>> InputsArr;
			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			FObj->SetArrayField(TEXT("inputs"), InputsArr);
		}
		else
		{
			FObj->SetBoolField(TEXT("is_pure"), false);
			FObj->SetBoolField(TEXT("is_const"), false);
			FObj->SetBoolField(TEXT("is_static"), false);
			FObj->SetBoolField(TEXT("call_in_editor"), false);
			FObj->SetStringField(TEXT("category"), TEXT(""));
			FObj->SetStringField(TEXT("description"), TEXT(""));
			FObj->SetStringField(TEXT("access_specifier"), TEXT("Public"));
			FObj->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
		}

		// Outputs: input pins on the result node (excluding exec)
		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		if (ResultNode)
		{
			for (const UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				OutputsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		FObj->SetArrayField(TEXT("outputs"), OutputsArr);

		FuncsArr.Add(MakeShared<FJsonValueObject>(FObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("functions"), FuncsArr);
	Root->SetNumberField(TEXT("function_count"), FuncsArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_event_dispatchers ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetEventDispatchers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> DispatchersArr;

	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();

		// Strip the "_Signature" suffix that UE appends to delegate graph names
		FString RawName = Graph->GetName();
		FString DisplayName = RawName;
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		DObj->SetStringField(TEXT("name"), DisplayName);

		// Signature pins from the entry node (excluding exec)
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!EntryNode) continue;

			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PObj->SetStringField(TEXT("type"),
					MonolithBlueprintInternal::ContainerPrefix(Pin->PinType) +
					MonolithBlueprintInternal::PinTypeToString(Pin->PinType));
				PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			break; // only one entry node per graph
		}
		DObj->SetArrayField(TEXT("signature_pins"), PinsArr);
		DispatchersArr.Add(MakeShared<FJsonValueObject>(DObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("event_dispatchers"), DispatchersArr);
	Root->SetNumberField(TEXT("count"), DispatchersArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_parent_class ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetParentClass(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Parent class info
	if (BP->ParentClass)
	{
		TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
		ParentObj->SetStringField(TEXT("name"), BP->ParentClass->GetName());
		ParentObj->SetStringField(TEXT("path"), BP->ParentClass->GetPathName());
		Root->SetObjectField(TEXT("parent_class"), ParentObj);
	}

	// Blueprint type
	FString BPTypeStr;
	switch (BP->BlueprintType)
	{
	case BPTYPE_Normal:          BPTypeStr = TEXT("Normal"); break;
	case BPTYPE_Const:           BPTypeStr = TEXT("Const"); break;
	case BPTYPE_MacroLibrary:    BPTypeStr = TEXT("MacroLibrary"); break;
	case BPTYPE_Interface:       BPTypeStr = TEXT("Interface"); break;
	case BPTYPE_LevelScript:     BPTypeStr = TEXT("LevelScript"); break;
	case BPTYPE_FunctionLibrary: BPTypeStr = TEXT("FunctionLibrary"); break;
	default:                     BPTypeStr = TEXT("Normal"); break;
	}
	Root->SetStringField(TEXT("blueprint_type"), BPTypeStr);

	// Status
	FString StatusStr;
	switch (BP->Status)
	{
	case BS_Unknown:       StatusStr = TEXT("Unknown"); break;
	case BS_Dirty:         StatusStr = TEXT("Dirty"); break;
	case BS_Error:         StatusStr = TEXT("Error"); break;
	case BS_UpToDate:      StatusStr = TEXT("UpToDate"); break;
	case BS_BeingCreated:  StatusStr = TEXT("BeingCreated"); break;
	default:               StatusStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("status"), StatusStr);

	// Generated class name
	if (BP->GeneratedClass)
	{
		Root->SetStringField(TEXT("generated_class_name"), BP->GeneratedClass->GetName());
	}

	// Flags
	Root->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(BP));
	Root->SetBoolField(TEXT("is_actor_based"), FBlueprintEditorUtils::IsActorBased(BP));

	return FMonolithActionResult::Success(Root);
}

// --- get_interfaces ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetInterfaces(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> InterfacesArr;

	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (!Iface.Interface) continue;

		TSharedPtr<FJsonObject> IObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), Iface.Interface->GetName());
		ClassObj->SetStringField(TEXT("path"), Iface.Interface->GetPathName());
		IObj->SetObjectField(TEXT("interface_class"), ClassObj);
		IObj->SetBoolField(TEXT("is_inherited"), false);
		InterfacesArr.Add(MakeShared<FJsonValueObject>(IObj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("interfaces"), InterfacesArr);
	Root->SetNumberField(TEXT("count"), InterfacesArr.Num());
	return FMonolithActionResult::Success(Root);
}

// --- get_construction_script ---

FMonolithActionResult FMonolithBlueprintActions::HandleGetConstructionScript(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = LoadBlueprint(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UEdGraph* CSGraph = FBlueprintEditorUtils::FindUserConstructionScript(BP);
	if (!CSGraph)
	{
		return FMonolithActionResult::Error(TEXT("No Construction Script found (Blueprint may not have one)"));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), CSGraph->GetName());
	Root->SetStringField(TEXT("graph_type"), TEXT("construction_script"));

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (UEdGraphNode* Node : CSGraph->Nodes)
	{
		if (!Node) continue;
		NodesArr.Add(MakeShared<FJsonValueObject>(MonolithBlueprintInternal::SerializeNode(Node)));
	}
	Root->SetArrayField(TEXT("nodes"), NodesArr);
	Root->SetNumberField(TEXT("node_count"), NodesArr.Num());

	return FMonolithActionResult::Success(Root);
}
