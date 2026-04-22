#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "Engine/UserDefinedEnum.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * 自定义枚举的关键不是复杂图结构，而是“有哪些枚举项、每项值是多少”。
 *
 * 所以这里把枚举项既写进 node 的 entries 摘要，
 * 也拆成 variables，方便全文搜索和逐项比较。
 */

namespace UserDefinedEnumIndexerInternal
{
	static bool BuildPayload(UUserDefinedEnum* UserDefinedEnum, MonolithSimpleArtifactSerialization::FNodeVariablePayload& OutPayload)
	{
		if (!UserDefinedEnum)
		{
			return false;
		}

		auto Props = MakeShared<FJsonObject>();
		const int32 NumEnums = UserDefinedEnum->NumEnums();
		const int32 EntryCount = FMath::Max(0, NumEnums - 1);
		Props->SetNumberField(TEXT("entry_count"), EntryCount);

		TArray<TSharedPtr<FJsonValue>> Entries;
		OutPayload = MonolithSimpleArtifactSerialization::FNodeVariablePayload();
		OutPayload.Variables.Reserve(EntryCount);

		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			// UE 的最后一个枚举项通常是隐藏/哨兵项，所以这里只处理真实业务项。
			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), UserDefinedEnum->GetNameByIndex(Index).ToString());
			Entry->SetStringField(TEXT("display_name"), UserDefinedEnum->GetDisplayNameTextByIndex(Index).ToString());
			Entry->SetNumberField(TEXT("value"), UserDefinedEnum->GetValueByIndex(Index));
			Entries.Add(MakeShared<FJsonValueObject>(Entry));

			FIndexedVariable Variable;
			Variable.VarName = UserDefinedEnum->GetNameByIndex(Index).ToString();
			Variable.VarType = TEXT("EnumEntry");
			Variable.Category = UserDefinedEnum->GetName();
			Variable.DefaultValue = FString::Printf(TEXT("%lld"), UserDefinedEnum->GetValueByIndex(Index));
			OutPayload.Variables.Add(MoveTemp(Variable));
		}

		Props->SetArrayField(TEXT("entries"), Entries);

		OutPayload.Node.NodeName = UserDefinedEnum->GetName();
		OutPayload.Node.NodeClass = TEXT("UserDefinedEnum");
		OutPayload.Node.NodeType = TEXT("Enum");

		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutPayload.Node.Properties);
		return FJsonSerializer::Serialize(Props, *Writer, true);
	}
}

bool FUserDefinedEnumIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!UserDefinedEnumIndexerInternal::BuildPayload(Cast<UUserDefinedEnum>(LoadedAsset), Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeNodeVariablePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FUserDefinedEnumIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayload(Payload, DB, AssetId);
}

bool FUserDefinedEnumIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayloadToShadow(Payload, DB, AssetId, CohortName);
}
