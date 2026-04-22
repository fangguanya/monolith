#include "Indexers/MaterialIndexer.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MonolithIndexerShadowMode.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * Material 的 artifact 和 Blueprint 很相似：
 * - nodes
 * - parameters
 * - connections
 *
 * 但这里更强调“着色配置摘要”，比如参数默认值、参数来源、表达式节点类型等。
 */

namespace MaterialIndexerInternal
{
	struct FMaterialArtifactNode
	{
		FString NodeType;
		FString NodeName;
		FString NodeClass;
		FString Properties = TEXT("{}");
		int32 PosX = 0;
		int32 PosY = 0;
	};

	struct FMaterialArtifactParameter
	{
		FString ParamName;
		FString ParamType;
		FString ParamGroup;
		FString DefaultValue;
		FString Source;
	};

	struct FMaterialArtifactConnection
	{
		int32 SourceNodeIndex = INDEX_NONE;
		FString SourcePin;
		int32 TargetNodeIndex = INDEX_NONE;
		FString TargetPin;
		FString PinType;
	};

	struct FMaterialArtifactPayload
	{
		TArray<FMaterialArtifactNode> Nodes;
		TArray<FMaterialArtifactParameter> Parameters;
		TArray<FMaterialArtifactConnection> Connections;
	};

	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	static void SerializePayload(const FMaterialArtifactPayload& Payload, TArray<uint8>& OutBytes)
	{
		// 同样使用 JSON 中转，优先保证结构清楚、便于排查。
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1.0);

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		NodeValues.Reserve(Payload.Nodes.Num());
		for (const FMaterialArtifactNode& Node : Payload.Nodes)
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

		TArray<TSharedPtr<FJsonValue>> ParameterValues;
		ParameterValues.Reserve(Payload.Parameters.Num());
		for (const FMaterialArtifactParameter& Parameter : Payload.Parameters)
		{
			TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
			ParameterObject->SetStringField(TEXT("param_name"), Parameter.ParamName);
			ParameterObject->SetStringField(TEXT("param_type"), Parameter.ParamType);
			ParameterObject->SetStringField(TEXT("param_group"), Parameter.ParamGroup);
			ParameterObject->SetStringField(TEXT("default_value"), Parameter.DefaultValue);
			ParameterObject->SetStringField(TEXT("source"), Parameter.Source);
			ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
		}
		Root->SetArrayField(TEXT("parameters"), ParameterValues);

		TArray<TSharedPtr<FJsonValue>> ConnectionValues;
		ConnectionValues.Reserve(Payload.Connections.Num());
		for (const FMaterialArtifactConnection& Connection : Payload.Connections)
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

	static bool DeserializePayload(const TArray<uint8>& Bytes, FMaterialArtifactPayload& OutPayload)
	{
		OutPayload = FMaterialArtifactPayload();
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

				FMaterialArtifactNode Node;
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

		const TArray<TSharedPtr<FJsonValue>>* ParameterValues = nullptr;
		if (Root->TryGetArrayField(TEXT("parameters"), ParameterValues) && ParameterValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ParameterValues)
			{
				const TSharedPtr<FJsonObject>* ParameterObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(ParameterObject) || !ParameterObject || !ParameterObject->IsValid())
				{
					return false;
				}

				FMaterialArtifactParameter Parameter;
				if (!(*ParameterObject)->TryGetStringField(TEXT("param_name"), Parameter.ParamName)
					|| !(*ParameterObject)->TryGetStringField(TEXT("param_type"), Parameter.ParamType))
				{
					return false;
				}

				(*ParameterObject)->TryGetStringField(TEXT("param_group"), Parameter.ParamGroup);
				(*ParameterObject)->TryGetStringField(TEXT("default_value"), Parameter.DefaultValue);
				(*ParameterObject)->TryGetStringField(TEXT("source"), Parameter.Source);
				OutPayload.Parameters.Add(MoveTemp(Parameter));
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

				FMaterialArtifactConnection Connection;
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

	static FIndexedNode MakeIndexedNode(const FMaterialArtifactNode& NodePayload, const int64 AssetId)
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

	static FIndexedParameter MakeIndexedParameter(const FMaterialArtifactParameter& ParameterPayload, const int64 AssetId)
	{
		FIndexedParameter Parameter;
		Parameter.AssetId = AssetId;
		Parameter.ParamName = ParameterPayload.ParamName;
		Parameter.ParamType = ParameterPayload.ParamType;
		Parameter.ParamGroup = ParameterPayload.ParamGroup;
		Parameter.DefaultValue = ParameterPayload.DefaultValue;
		Parameter.Source = ParameterPayload.Source;
		return Parameter;
	}

	static void BuildExpressionPayload(const TArray<UMaterialExpression*>& Expressions, FMaterialArtifactPayload& OutPayload)
	{
		TMap<UMaterialExpression*, int32> ExpressionToNodeIndex;
		ExpressionToNodeIndex.Reserve(Expressions.Num());

		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression)
			{
				continue;
			}

			FMaterialArtifactNode Node;
			Node.NodeName = Expression->GetName();
			Node.NodeClass = Expression->GetClass()->GetName();
			Node.PosX = Expression->MaterialExpressionEditorX;
			Node.PosY = Expression->MaterialExpressionEditorY;

			if (UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				Node.NodeType = TEXT("ScalarParameter");

				FMaterialArtifactParameter Parameter;
				Parameter.ParamName = ScalarParameter->ParameterName.ToString();
				Parameter.ParamType = TEXT("Scalar");
				Parameter.ParamGroup = ScalarParameter->Group.ToString();
				Parameter.DefaultValue = FString::SanitizeFloat(ScalarParameter->DefaultValue);
				Parameter.Source = TEXT("Material");
				OutPayload.Parameters.Add(MoveTemp(Parameter));

				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetStringField(TEXT("parameter_name"), ScalarParameter->ParameterName.ToString());
				PropertiesObject->SetNumberField(TEXT("default_value"), ScalarParameter->DefaultValue);
				SerializeJsonObject(PropertiesObject, Node.Properties);
			}
			else if (UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				Node.NodeType = TEXT("VectorParameter");

				FMaterialArtifactParameter Parameter;
				Parameter.ParamName = VectorParameter->ParameterName.ToString();
				Parameter.ParamType = TEXT("Vector");
				Parameter.ParamGroup = VectorParameter->Group.ToString();
				Parameter.DefaultValue = VectorParameter->DefaultValue.ToString();
				Parameter.Source = TEXT("Material");
				OutPayload.Parameters.Add(MoveTemp(Parameter));
			}
			else if (UMaterialExpressionTextureObjectParameter* TextureParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
			{
				Node.NodeType = TEXT("TextureParameter");

				FMaterialArtifactParameter Parameter;
				Parameter.ParamName = TextureParameter->ParameterName.ToString();
				Parameter.ParamType = TEXT("Texture");
				Parameter.ParamGroup = TextureParameter->Group.ToString();
				Parameter.DefaultValue = TextureParameter->Texture ? TextureParameter->Texture->GetPathName() : FString();
				Parameter.Source = TEXT("Material");
				OutPayload.Parameters.Add(MoveTemp(Parameter));
			}
			else if (UMaterialExpressionStaticBoolParameter* BoolParameter = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
			{
				Node.NodeType = TEXT("StaticBoolParameter");

				FMaterialArtifactParameter Parameter;
				Parameter.ParamName = BoolParameter->ParameterName.ToString();
				Parameter.ParamType = TEXT("StaticBool");
				Parameter.ParamGroup = BoolParameter->Group.ToString();
				Parameter.DefaultValue = BoolParameter->DefaultValue ? TEXT("true") : TEXT("false");
				Parameter.Source = TEXT("Material");
				OutPayload.Parameters.Add(MoveTemp(Parameter));
			}
			else if (Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				Node.NodeType = TEXT("FunctionInput");
			}
			else if (Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				Node.NodeType = TEXT("FunctionOutput");
			}
			else
			{
				Node.NodeType = TEXT("Expression");
			}

			const int32 NodeIndex = OutPayload.Nodes.Add(MoveTemp(Node));
			ExpressionToNodeIndex.Add(Expression, NodeIndex);
		}

		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression)
			{
				continue;
			}

			const int32* TargetNodeIndex = ExpressionToNodeIndex.Find(Expression);
			if (!TargetNodeIndex)
			{
				continue;
			}

			int32 InputIndex = 0;
			for (FExpressionInputIterator Iterator(Expression); Iterator; ++Iterator, ++InputIndex)
			{
				if (!Iterator->Expression)
				{
					continue;
				}

				const int32* SourceNodeIndex = ExpressionToNodeIndex.Find(Iterator->Expression);
				if (!SourceNodeIndex)
				{
					continue;
				}

				FMaterialArtifactConnection Connection;
				Connection.SourceNodeIndex = *SourceNodeIndex;
				Connection.SourcePin = FString::Printf(TEXT("Output_%d"), Iterator->OutputIndex);
				Connection.TargetNodeIndex = *TargetNodeIndex;
				Connection.TargetPin = FString::Printf(TEXT("Input_%d"), InputIndex);
				Connection.PinType = TEXT("Material");
				OutPayload.Connections.Add(MoveTemp(Connection));
			}
		}
	}

	static void BuildMaterialInstancePayload(UMaterialInstanceConstant* MaterialInstance, FMaterialArtifactPayload& OutPayload)
	{
		if (!MaterialInstance)
		{
			return;
		}

		for (const FScalarParameterValue& ScalarParameter : MaterialInstance->ScalarParameterValues)
		{
			FMaterialArtifactParameter Parameter;
			Parameter.ParamName = ScalarParameter.ParameterInfo.Name.ToString();
			Parameter.ParamType = TEXT("Scalar");
			Parameter.DefaultValue = FString::SanitizeFloat(ScalarParameter.ParameterValue);
			Parameter.Source = TEXT("MaterialInstance");
			OutPayload.Parameters.Add(MoveTemp(Parameter));
		}

		for (const FVectorParameterValue& VectorParameter : MaterialInstance->VectorParameterValues)
		{
			FMaterialArtifactParameter Parameter;
			Parameter.ParamName = VectorParameter.ParameterInfo.Name.ToString();
			Parameter.ParamType = TEXT("Vector");
			Parameter.DefaultValue = VectorParameter.ParameterValue.ToString();
			Parameter.Source = TEXT("MaterialInstance");
			OutPayload.Parameters.Add(MoveTemp(Parameter));
		}

		for (const FTextureParameterValue& TextureParameter : MaterialInstance->TextureParameterValues)
		{
			FMaterialArtifactParameter Parameter;
			Parameter.ParamName = TextureParameter.ParameterInfo.Name.ToString();
			Parameter.ParamType = TEXT("Texture");
			Parameter.DefaultValue = TextureParameter.ParameterValue ? TextureParameter.ParameterValue->GetPathName() : FString();
			Parameter.Source = TEXT("MaterialInstance");
			OutPayload.Parameters.Add(MoveTemp(Parameter));
		}
	}

	static bool BuildPayload(UObject* LoadedAsset, FMaterialArtifactPayload& OutPayload)
	{
		OutPayload = FMaterialArtifactPayload();

		if (UMaterial* Material = Cast<UMaterial>(LoadedAsset))
		{
			TArray<UMaterialExpression*> Expressions;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				Expressions.Add(Expression);
			}
			BuildExpressionPayload(Expressions, OutPayload);
			return true;
		}

		if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(LoadedAsset))
		{
			BuildMaterialInstancePayload(MaterialInstance, OutPayload);
			return true;
		}

		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(LoadedAsset))
		{
			TArray<UMaterialExpression*> Expressions;
			for (UMaterialExpression* Expression : MaterialFunction->GetExpressions())
			{
				Expressions.Add(Expression);
			}
			BuildExpressionPayload(Expressions, OutPayload);
			return true;
		}

		return false;
	}

	static bool MaterializePayload(const FMaterialArtifactPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		TArray<int64> NodeIds;
		NodeIds.Reserve(Payload.Nodes.Num());

		for (const FMaterialArtifactNode& NodePayload : Payload.Nodes)
		{
			const int64 NodeId = DB.InsertNode(MakeIndexedNode(NodePayload, AssetId));
			if (NodeId < 0)
			{
				return false;
			}

			NodeIds.Add(NodeId);
		}

		for (const FMaterialArtifactParameter& ParameterPayload : Payload.Parameters)
		{
			if (DB.InsertParameter(MakeIndexedParameter(ParameterPayload, AssetId)) < 0)
			{
				return false;
			}
		}

		for (const FMaterialArtifactConnection& ConnectionPayload : Payload.Connections)
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

	static bool MaterializePayloadToShadow(const FMaterialArtifactPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId, const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		TArray<uint64> NodeRowHashes;
		ShadowNodes.Reserve(Payload.Nodes.Num());
		NodeRowHashes.Reserve(Payload.Nodes.Num());

		for (const FMaterialArtifactNode& NodePayload : Payload.Nodes)
		{
			FMonolithShadowIndexedNode ShadowNode;
			ShadowNode.Node = MakeIndexedNode(NodePayload, AssetId);
			ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);
			NodeRowHashes.Add(ShadowNode.RowHash);
			ShadowNodes.Add(MoveTemp(ShadowNode));
		}

		TArray<FMonolithShadowIndexedParameter> ShadowParameters;
		ShadowParameters.Reserve(Payload.Parameters.Num());
		for (const FMaterialArtifactParameter& ParameterPayload : Payload.Parameters)
		{
			FMonolithShadowIndexedParameter ShadowParameter;
			ShadowParameter.Parameter = MakeIndexedParameter(ParameterPayload, AssetId);
			ShadowParameter.RowHash = ComputeParameterRowHash(ShadowParameter.Parameter);
			ShadowParameters.Add(MoveTemp(ShadowParameter));
		}

		TArray<FMonolithShadowIndexedConnection> ShadowConnections;
		ShadowConnections.Reserve(Payload.Connections.Num());
		for (const FMaterialArtifactConnection& ConnectionPayload : Payload.Connections)
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
			&& DB.ReplaceShadowParametersForAsset(CohortName, AssetId, ShadowParameters)
			&& DB.ReplaceShadowConnectionsForAsset(CohortName, AssetId, ShadowConnections);
	}
}

bool FMaterialIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	MaterialIndexerInternal::FMaterialArtifactPayload Payload;
	if (!MaterialIndexerInternal::BuildPayload(LoadedAsset, Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MaterialIndexerInternal::SerializePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FMaterialIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MaterialIndexerInternal::FMaterialArtifactPayload Payload;
	if (!MaterialIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MaterialIndexerInternal::MaterializePayload(Payload, DB, AssetId);
}

bool FMaterialIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MaterialIndexerInternal::FMaterialArtifactPayload Payload;
	if (!MaterialIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MaterialIndexerInternal::MaterializePayloadToShadow(Payload, DB, AssetId, CohortName);
}
