#include "Indexers/EQSIndexer.h"

#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * EQS 的价值不只是“这个查询用了哪些类”，
 * 更重要的是“Option、Generator、Test 是怎样组合起来的”。
 *
 * 所以这次迁移里，我们把 EQS 改成真正的图 payload：
 * - 每个 Option 是一个节点；
 * - Generator 和每个 Test 也各自是节点；
 * - Option -> Generator / Test 的关系保存在内部 connections 里。
 */

namespace EQSIndexerInternal
{
	/** 把 JSON 对象压成紧凑字符串，方便直接放进 node.properties。 */
	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	/** 向 graph payload 追加一个节点。 */
	static int32 AddNode(
		MonolithSimpleArtifactSerialization::FGraphPayload& Payload,
		const FString& NodeType,
		const FString& NodeName,
		const FString& NodeClass,
		const FString& Properties)
	{
		FIndexedNode Node;
		Node.NodeType = NodeType;
		Node.NodeName = NodeName;
		Node.NodeClass = NodeClass;
		Node.Properties = Properties;
		return Payload.Nodes.Add(MoveTemp(Node));
	}

	/** 向 graph payload 追加一条内部连接。 */
	static void AddConnection(
		MonolithSimpleArtifactSerialization::FGraphPayload& Payload,
		const int32 SourceNodeIndex,
		const FString& SourcePin,
		const int32 TargetNodeIndex,
		const FString& TargetPin,
		const FString& PinType)
	{
		MonolithSimpleArtifactSerialization::FGraphPayloadConnection Connection;
		Connection.SourceNodeIndex = SourceNodeIndex;
		Connection.SourcePin = SourcePin;
		Connection.TargetNodeIndex = TargetNodeIndex;
		Connection.TargetPin = TargetPin;
		Connection.PinType = PinType;
		Payload.Connections.Add(MoveTemp(Connection));
	}

	/** 构建 Option 节点属性。 */
	static bool BuildOptionProperties(const int32 OptionIndex, const int32 TestCount, const bool bHasGenerator, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("option_index"), OptionIndex);
		Properties->SetNumberField(TEXT("test_count"), TestCount);
		Properties->SetBoolField(TEXT("has_generator"), bHasGenerator);
		return SerializeJsonObject(Properties, OutProperties);
	}

	/** 构建 Generator 节点属性。 */
	static bool BuildGeneratorProperties(const int32 OptionIndex, const UEnvQueryGenerator* Generator, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("option_index"), OptionIndex);
		Properties->SetStringField(TEXT("class"), Generator ? Generator->GetClass()->GetName() : TEXT("None"));
		return SerializeJsonObject(Properties, OutProperties);
	}

	/** 构建 Test 节点属性。 */
	static bool BuildTestProperties(const int32 OptionIndex, const int32 TestIndex, const UEnvQueryTest* Test, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("option_index"), OptionIndex);
		Properties->SetNumberField(TEXT("test_index"), TestIndex);
		Properties->SetStringField(TEXT("class"), Test ? Test->GetClass()->GetName() : TEXT("None"));
		return SerializeJsonObject(Properties, OutProperties);
	}
}

bool FEQSIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!BuildPayload(Cast<UEnvQuery>(LoadedAsset), Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeGraphPayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FEQSIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayload(Payload, DB, AssetId);
}

bool FEQSIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FEQSIndexer::BuildPayload(UEnvQuery* Query, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const
{
	OutPayload = MonolithSimpleArtifactSerialization::FGraphPayload();
	if (!Query)
	{
		return false;
	}

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
	for (int32 OptionIndex = 0; OptionIndex < Options.Num(); ++OptionIndex)
	{
		const UEnvQueryOption* Option = Options[OptionIndex];
		if (!Option)
		{
			continue;
		}

		FString OptionProperties;
		if (!EQSIndexerInternal::BuildOptionProperties(OptionIndex, Option->Tests.Num(), Option->Generator != nullptr, OptionProperties))
		{
			return false;
		}

		const int32 OptionNodeIndex = EQSIndexerInternal::AddNode(
			OutPayload,
			TEXT("EQS_Option"),
			FString::Printf(TEXT("Option_%d"), OptionIndex),
			TEXT("EnvQueryOption"),
			OptionProperties);

		if (Option->Generator)
		{
			FString GeneratorProperties;
			if (!EQSIndexerInternal::BuildGeneratorProperties(OptionIndex, Option->Generator, GeneratorProperties))
			{
				return false;
			}

			const int32 GeneratorNodeIndex = EQSIndexerInternal::AddNode(
				OutPayload,
				TEXT("EQS_Generator"),
				Option->Generator->GetClass()->GetName(),
				Option->Generator->GetClass()->GetName(),
				GeneratorProperties);
			EQSIndexerInternal::AddConnection(
				OutPayload,
				OptionNodeIndex,
				TEXT("Generator"),
				GeneratorNodeIndex,
				TEXT("Self"),
				TEXT("EQS_Generator"));
		}

		for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
		{
			const UEnvQueryTest* Test = Option->Tests[TestIndex];
			if (!Test)
			{
				continue;
			}

			FString TestProperties;
			if (!EQSIndexerInternal::BuildTestProperties(OptionIndex, TestIndex, Test, TestProperties))
			{
				return false;
			}

			const int32 TestNodeIndex = EQSIndexerInternal::AddNode(
				OutPayload,
				TEXT("EQS_Test"),
				FString::Printf(TEXT("Option_%d_Test_%d"), OptionIndex, TestIndex),
				Test->GetClass()->GetName(),
				TestProperties);
			EQSIndexerInternal::AddConnection(
				OutPayload,
				OptionNodeIndex,
				TEXT("Tests"),
				TestNodeIndex,
				FString::Printf(TEXT("Test_%d"), TestIndex),
				TEXT("EQS_Test"));
		}
	}

	return true;
}
