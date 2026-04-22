#include "Indexers/BlueprintIndexer.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "MonolithIndexerShadowMode.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * Blueprint 的 artifact 载荷比较丰富：
 * - nodes
 * - variables
 * - connections
 *
 * 这里自己维护一份轻量中间结构体，
 * 目的是把“UE 蓝图对象图”先整理成稳定、可序列化、可比较的纯数据。
 */

namespace BlueprintIndexerInternal
{
	struct FBlueprintArtifactNode
	{
		/** 节点大类，例如 Event、CallFunction。 */
		FString NodeType;
		/** 节点显示名。 */
		FString NodeName;
		/** 节点具体类名。 */
		FString NodeClass;
		/** 额外属性 JSON。 */
		FString Properties = TEXT("{}");
		/** 节点在编辑器里的 X。 */
		int32 PosX = 0;
		/** 节点在编辑器里的 Y。 */
		int32 PosY = 0;
	};

	struct FBlueprintArtifactVariable
	{
		FString VarName;
		FString VarType;
		FString Category;
		FString DefaultValue;
		bool bIsExposed = false;
		bool bIsReplicated = false;
	};

	struct FBlueprintArtifactConnection
	{
		int32 SourceNodeIndex = INDEX_NONE;
		FString SourcePin;
		int32 TargetNodeIndex = INDEX_NONE;
		FString TargetPin;
		FString PinType;
	};

	struct FBlueprintArtifactPayload
	{
		TArray<FBlueprintArtifactNode> Nodes;
		TArray<FBlueprintArtifactVariable> Variables;
		TArray<FBlueprintArtifactConnection> Connections;
	};

	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	static void SerializePayload(const FBlueprintArtifactPayload& Payload, TArray<uint8>& OutBytes)
	{
		// 这里选 JSON 而不是手写二进制字段布局，
		// 是为了让调试和未来 schema 扩展更直观。
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1.0);

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		NodeValues.Reserve(Payload.Nodes.Num());
		for (const FBlueprintArtifactNode& Node : Payload.Nodes)
		{
			TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("node_type"), Node.NodeType);
			NodeObject->SetStringField(TEXT("node_name"), Node.NodeName);
			NodeObject->SetStringField(TEXT("node_class"), Node.NodeClass);
			NodeObject->SetStringField(TEXT("properties"), Node.Properties);
			NodeObject->SetNumberField(TEXT("pos_x"), static_cast<double>(Node.PosX));
			NodeObject->SetNumberField(TEXT("pos_y"), static_cast<double>(Node.PosY));
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
		}
		Root->SetArrayField(TEXT("nodes"), NodeValues);

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		VariableValues.Reserve(Payload.Variables.Num());
		for (const FBlueprintArtifactVariable& Variable : Payload.Variables)
		{
			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("var_name"), Variable.VarName);
			VariableObject->SetStringField(TEXT("var_type"), Variable.VarType);
			VariableObject->SetStringField(TEXT("category"), Variable.Category);
			VariableObject->SetStringField(TEXT("default_value"), Variable.DefaultValue);
			VariableObject->SetBoolField(TEXT("is_exposed"), Variable.bIsExposed);
			VariableObject->SetBoolField(TEXT("is_replicated"), Variable.bIsReplicated);
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
		}
		Root->SetArrayField(TEXT("variables"), VariableValues);

		TArray<TSharedPtr<FJsonValue>> ConnectionValues;
		ConnectionValues.Reserve(Payload.Connections.Num());
		for (const FBlueprintArtifactConnection& Connection : Payload.Connections)
		{
			TSharedPtr<FJsonObject> ConnectionObject = MakeShared<FJsonObject>();
			ConnectionObject->SetNumberField(TEXT("source_node_index"), static_cast<double>(Connection.SourceNodeIndex));
			ConnectionObject->SetStringField(TEXT("source_pin"), Connection.SourcePin);
			ConnectionObject->SetNumberField(TEXT("target_node_index"), static_cast<double>(Connection.TargetNodeIndex));
			ConnectionObject->SetStringField(TEXT("target_pin"), Connection.TargetPin);
			ConnectionObject->SetStringField(TEXT("pin_type"), Connection.PinType);
			ConnectionValues.Add(MakeShared<FJsonValueObject>(ConnectionObject));
		}
		Root->SetArrayField(TEXT("connections"), ConnectionValues);

		FString Json;
		if (!SerializeJsonObject(Root, Json))
		{
			OutBytes.Reset();
			return;
		}

		FTCHARToUTF8 Convert(*Json);
		OutBytes.Reset();
		if (Convert.Length() > 0)
		{
			OutBytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	static bool DeserializePayload(const TArray<uint8>& Bytes, FBlueprintArtifactPayload& OutPayload)
	{
		OutPayload = FBlueprintArtifactPayload();
		if (Bytes.Num() == 0)
		{
			return false;
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		const FString Json(Convert.Length(), Convert.Get());
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (Root->TryGetArrayField(TEXT("nodes"), NodeValues) && NodeValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *NodeValues)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					return false;
				}

				FBlueprintArtifactNode Node;
				if (!(*NodeObject)->TryGetStringField(TEXT("node_type"), Node.NodeType)
					|| !(*NodeObject)->TryGetStringField(TEXT("node_name"), Node.NodeName)
					|| !(*NodeObject)->TryGetStringField(TEXT("node_class"), Node.NodeClass))
				{
					return false;
				}

				(*NodeObject)->TryGetStringField(TEXT("properties"), Node.Properties);
				double PosX = 0.0;
				double PosY = 0.0;
				(*NodeObject)->TryGetNumberField(TEXT("pos_x"), PosX);
				(*NodeObject)->TryGetNumberField(TEXT("pos_y"), PosY);
				Node.PosX = static_cast<int32>(PosX);
				Node.PosY = static_cast<int32>(PosY);
				OutPayload.Nodes.Add(MoveTemp(Node));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* VariableValues = nullptr;
		if (Root->TryGetArrayField(TEXT("variables"), VariableValues) && VariableValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *VariableValues)
			{
				const TSharedPtr<FJsonObject>* VariableObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(VariableObject) || !VariableObject || !VariableObject->IsValid())
				{
					return false;
				}

				FBlueprintArtifactVariable Variable;
				if (!(*VariableObject)->TryGetStringField(TEXT("var_name"), Variable.VarName)
					|| !(*VariableObject)->TryGetStringField(TEXT("var_type"), Variable.VarType))
				{
					return false;
				}

				(*VariableObject)->TryGetStringField(TEXT("category"), Variable.Category);
				(*VariableObject)->TryGetStringField(TEXT("default_value"), Variable.DefaultValue);
				(*VariableObject)->TryGetBoolField(TEXT("is_exposed"), Variable.bIsExposed);
				(*VariableObject)->TryGetBoolField(TEXT("is_replicated"), Variable.bIsReplicated);
				OutPayload.Variables.Add(MoveTemp(Variable));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ConnectionValues = nullptr;
		if (Root->TryGetArrayField(TEXT("connections"), ConnectionValues) && ConnectionValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ConnectionValues)
			{
				const TSharedPtr<FJsonObject>* ConnectionObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(ConnectionObject) || !ConnectionObject || !ConnectionObject->IsValid())
				{
					return false;
				}

				FBlueprintArtifactConnection Connection;
				double SourceNodeIndex = 0.0;
				double TargetNodeIndex = 0.0;
				if (!(*ConnectionObject)->TryGetNumberField(TEXT("source_node_index"), SourceNodeIndex)
					|| !(*ConnectionObject)->TryGetNumberField(TEXT("target_node_index"), TargetNodeIndex)
					|| !(*ConnectionObject)->TryGetStringField(TEXT("source_pin"), Connection.SourcePin)
					|| !(*ConnectionObject)->TryGetStringField(TEXT("target_pin"), Connection.TargetPin))
				{
					return false;
				}

				(*ConnectionObject)->TryGetStringField(TEXT("pin_type"), Connection.PinType);
				Connection.SourceNodeIndex = static_cast<int32>(SourceNodeIndex);
				Connection.TargetNodeIndex = static_cast<int32>(TargetNodeIndex);
				OutPayload.Connections.Add(MoveTemp(Connection));
			}
		}

		return true;
	}

	static bool BuildFunctionCallProperties(UK2Node_CallFunction* FuncNode, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("function"), FuncNode->FunctionReference.GetMemberName().ToString());
		if (FuncNode->FunctionReference.GetMemberParentClass())
		{
			Properties->SetStringField(TEXT("target_class"), FuncNode->FunctionReference.GetMemberParentClass()->GetName());
		}
		return SerializeJsonObject(Properties, OutProperties);
	}

	static FBlueprintArtifactNode MakeNodePayload(UEdGraphNode* Node)
	{
		FBlueprintArtifactNode PayloadNode;
		PayloadNode.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		PayloadNode.NodeClass = Node->GetClass()->GetName();
		PayloadNode.PosX = Node->NodePosX;
		PayloadNode.PosY = Node->NodePosY;

		if (Cast<UK2Node_Event>(Node))
		{
			PayloadNode.NodeType = TEXT("Event");
		}
		else if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
		{
			PayloadNode.NodeType = TEXT("FunctionCall");
			BuildFunctionCallProperties(FuncNode, PayloadNode.Properties);
		}
		else if (Cast<UK2Node_VariableGet>(Node) || Cast<UK2Node_VariableSet>(Node))
		{
			PayloadNode.NodeType = TEXT("Variable");
		}
		else
		{
			PayloadNode.NodeType = TEXT("Other");
		}

		return PayloadNode;
	}

	static FBlueprintArtifactVariable MakeVariablePayload(UBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		FBlueprintArtifactVariable Variable;
		Variable.VarName = VarDesc.VarName.ToString();
		Variable.VarType = VarDesc.VarType.PinCategory.ToString();
		Variable.Category = VarDesc.Category.ToString();
		Variable.DefaultValue = VarDesc.DefaultValue;
		if (Variable.DefaultValue.IsEmpty() && Blueprint && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			if (CDO)
			{
				FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(VarDesc.VarName);
				if (Prop)
				{
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
					Prop->ExportTextItem_Direct(Variable.DefaultValue, ValuePtr, nullptr, CDO, PPF_None);
				}
			}
		}
		Variable.bIsExposed = !!(VarDesc.PropertyFlags & CPF_ExposeOnSpawn);
		Variable.bIsReplicated = !!(VarDesc.PropertyFlags & CPF_Net);
		return Variable;
	}

	static void BuildGraphPayload(UEdGraph* Graph, FBlueprintArtifactPayload& OutPayload)
	{
		if (!Graph)
		{
			return;
		}

		TMap<UEdGraphNode*, int32> NodeToPayloadIndex;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			const int32 NodeIndex = OutPayload.Nodes.Add(MakeNodePayload(Node));
			NodeToPayloadIndex.Add(Node, NodeIndex);
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			const int32* SourceNodeIndex = NodeToPayloadIndex.Find(Node);
			if (!SourceNodeIndex)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode())
					{
						continue;
					}

					const int32* TargetNodeIndex = NodeToPayloadIndex.Find(LinkedPin->GetOwningNode());
					if (!TargetNodeIndex)
					{
						continue;
					}

					FBlueprintArtifactConnection Connection;
					Connection.SourceNodeIndex = *SourceNodeIndex;
					Connection.SourcePin = Pin->PinName.ToString();
					Connection.TargetNodeIndex = *TargetNodeIndex;
					Connection.TargetPin = LinkedPin->PinName.ToString();
					Connection.PinType = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
						? TEXT("Exec")
						: Pin->PinType.PinCategory.ToString();
					OutPayload.Connections.Add(MoveTemp(Connection));
				}
			}
		}
	}

	static bool BuildPayload(UBlueprint* Blueprint, FBlueprintArtifactPayload& OutPayload)
	{
		OutPayload = FBlueprintArtifactPayload();
		if (!Blueprint)
		{
			return false;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		AllGraphs.RemoveAll([](const UEdGraph* Graph)
		{
			return Graph == nullptr;
		});
		AllGraphs.Sort([](const UEdGraph& A, const UEdGraph& B)
		{
			return A.GetName() < B.GetName();
		});

		for (UEdGraph* Graph : AllGraphs)
		{
			BuildGraphPayload(Graph, OutPayload);
		}

		for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
		{
			OutPayload.Variables.Add(MakeVariablePayload(Blueprint, VarDesc));
		}

		return true;
	}

	static FIndexedNode MakeIndexedNode(const FBlueprintArtifactNode& NodePayload, const int64 AssetId)
	{
		FIndexedNode Node;
		Node.AssetId = AssetId;
		Node.NodeType = NodePayload.NodeType;
		Node.NodeName = NodePayload.NodeName;
		Node.NodeClass = NodePayload.NodeClass;
		Node.Properties = NodePayload.Properties;
		Node.PosX = NodePayload.PosX;
		Node.PosY = NodePayload.PosY;
		return Node;
	}

	static FIndexedVariable MakeIndexedVariable(const FBlueprintArtifactVariable& VariablePayload, const int64 AssetId)
	{
		FIndexedVariable Variable;
		Variable.AssetId = AssetId;
		Variable.VarName = VariablePayload.VarName;
		Variable.VarType = VariablePayload.VarType;
		Variable.Category = VariablePayload.Category;
		Variable.DefaultValue = VariablePayload.DefaultValue;
		Variable.bIsExposed = VariablePayload.bIsExposed;
		Variable.bIsReplicated = VariablePayload.bIsReplicated;
		return Variable;
	}

	static bool MaterializePayload(const FBlueprintArtifactPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		TArray<int64> NodeIds;
		NodeIds.Reserve(Payload.Nodes.Num());

		for (const FBlueprintArtifactNode& NodePayload : Payload.Nodes)
		{
			const int64 NodeId = DB.InsertNode(MakeIndexedNode(NodePayload, AssetId));
			if (NodeId < 0)
			{
				return false;
			}
			NodeIds.Add(NodeId);
		}

		for (const FBlueprintArtifactVariable& VariablePayload : Payload.Variables)
		{
			if (DB.InsertVariable(MakeIndexedVariable(VariablePayload, AssetId)) < 0)
			{
				return false;
			}
		}

		for (const FBlueprintArtifactConnection& ConnectionPayload : Payload.Connections)
		{
			if (!NodeIds.IsValidIndex(ConnectionPayload.SourceNodeIndex) || !NodeIds.IsValidIndex(ConnectionPayload.TargetNodeIndex))
			{
				return false;
			}

			FIndexedConnection Connection;
			Connection.SourceNodeId = NodeIds[ConnectionPayload.SourceNodeIndex];
			Connection.SourcePin = ConnectionPayload.SourcePin;
			Connection.TargetNodeId = NodeIds[ConnectionPayload.TargetNodeIndex];
			Connection.TargetPin = ConnectionPayload.TargetPin;
			Connection.PinType = ConnectionPayload.PinType;
			if (DB.InsertConnection(Connection) < 0)
			{
				return false;
			}
		}

		return true;
	}

	static bool MaterializePayloadToShadow(const FBlueprintArtifactPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId, const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		TArray<uint64> NodeRowHashes;
		ShadowNodes.Reserve(Payload.Nodes.Num());
		NodeRowHashes.Reserve(Payload.Nodes.Num());

		for (const FBlueprintArtifactNode& NodePayload : Payload.Nodes)
		{
			FMonolithShadowIndexedNode ShadowNode;
			ShadowNode.Node = MakeIndexedNode(NodePayload, AssetId);
			ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);
			NodeRowHashes.Add(ShadowNode.RowHash);
			ShadowNodes.Add(MoveTemp(ShadowNode));
		}

		TArray<FMonolithShadowIndexedVariable> ShadowVariables;
		ShadowVariables.Reserve(Payload.Variables.Num());
		for (const FBlueprintArtifactVariable& VariablePayload : Payload.Variables)
		{
			FMonolithShadowIndexedVariable ShadowVariable;
			ShadowVariable.Variable = MakeIndexedVariable(VariablePayload, AssetId);
			ShadowVariable.RowHash = ComputeVariableRowHash(ShadowVariable.Variable);
			ShadowVariables.Add(MoveTemp(ShadowVariable));
		}

		TArray<FMonolithShadowIndexedConnection> ShadowConnections;
		ShadowConnections.Reserve(Payload.Connections.Num());
		for (const FBlueprintArtifactConnection& ConnectionPayload : Payload.Connections)
		{
			if (!NodeRowHashes.IsValidIndex(ConnectionPayload.SourceNodeIndex) || !NodeRowHashes.IsValidIndex(ConnectionPayload.TargetNodeIndex))
			{
				return false;
			}

			FMonolithShadowIndexedConnection ShadowConnection;
			ShadowConnection.SourceNodeRowHash = NodeRowHashes[ConnectionPayload.SourceNodeIndex];
			ShadowConnection.SourcePin = ConnectionPayload.SourcePin;
			ShadowConnection.TargetNodeRowHash = NodeRowHashes[ConnectionPayload.TargetNodeIndex];
			ShadowConnection.TargetPin = ConnectionPayload.TargetPin;
			ShadowConnection.PinType = ConnectionPayload.PinType;
			ShadowConnection.RowHash = ComputeConnectionRowHash(
				ShadowConnection.SourceNodeRowHash,
				ShadowConnection.SourcePin,
				ShadowConnection.TargetNodeRowHash,
				ShadowConnection.TargetPin,
				ShadowConnection.PinType);
			ShadowConnections.Add(MoveTemp(ShadowConnection));
		}

		return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes)
			&& DB.ReplaceShadowVariablesForAsset(CohortName, AssetId, ShadowVariables)
			&& DB.ReplaceShadowConnectionsForAsset(CohortName, AssetId, ShadowConnections);
	}
}

bool FBlueprintIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	BlueprintIndexerInternal::FBlueprintArtifactPayload Payload;
	if (!BlueprintIndexerInternal::BuildPayload(Cast<UBlueprint>(LoadedAsset), Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	BlueprintIndexerInternal::SerializePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FBlueprintIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	BlueprintIndexerInternal::FBlueprintArtifactPayload Payload;
	if (!BlueprintIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return BlueprintIndexerInternal::MaterializePayload(Payload, DB, AssetId);
}

bool FBlueprintIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	BlueprintIndexerInternal::FBlueprintArtifactPayload Payload;
	if (!BlueprintIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return BlueprintIndexerInternal::MaterializePayloadToShadow(Payload, DB, AssetId, CohortName);
}
