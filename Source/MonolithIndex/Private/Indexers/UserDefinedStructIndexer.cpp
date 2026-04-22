#include "Indexers/UserDefinedStructIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * 这份实现文件把用户自定义 Struct 压成 node + variables。
 *
 * 难点在于默认值不一定总能安全读取：
 * - 结构体可能还是 transient；
 * - 也可能状态不是 UpToDate。
 *
 * 所以这里会先检查 DefaultInstance 是否真的可读，再决定要不要导出默认值。
 */

namespace UserDefinedStructIndexerInternal
{
	static bool BuildPayload(UUserDefinedStruct* UserDefinedStruct, MonolithSimpleArtifactSerialization::FNodeVariablePayload& OutPayload)
	{
		if (!UserDefinedStruct)
		{
			return false;
		}

		const bool bCanReadDefaultInstance =
			!UserDefinedStruct->HasAnyFlags(RF_Transient)
			&& UserDefinedStruct->Status == UDSS_UpToDate;
		const void* DefaultInstance = bCanReadDefaultInstance ? UserDefinedStruct->GetDefaultInstance() : nullptr;
		auto Props = MakeShared<FJsonObject>();
		int32 FieldCount = 0;

		TArray<TSharedPtr<FJsonValue>> Fields;
		OutPayload = MonolithSimpleArtifactSerialization::FNodeVariablePayload();

		for (TFieldIterator<FProperty> It(UserDefinedStruct); It; ++It)
		{
			// 每个字段同时进入 fields 摘要和 variables。
			// 前者适合看整体结构，后者适合做搜索和差异比较。
			FProperty* Property = *It;
			++FieldCount;

			auto Field = MakeShared<FJsonObject>();
			Field->SetStringField(TEXT("name"), Property->GetName());
			Field->SetStringField(TEXT("type"), Property->GetCPPType());
			Field->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));

			FIndexedVariable Variable;
			Variable.VarName = Property->GetName();
			Variable.VarType = Property->GetCPPType();
			Variable.Category = Property->GetMetaData(TEXT("Category"));

			if (DefaultInstance)
			{
				FString DefaultValue;
				Property->ExportTextItem_Direct(
					DefaultValue,
					Property->ContainerPtrToValuePtr<void>(DefaultInstance),
					nullptr,
					nullptr,
					PPF_None);
				Field->SetStringField(TEXT("default_value"), DefaultValue);
				Variable.DefaultValue = DefaultValue;
			}

			Fields.Add(MakeShared<FJsonValueObject>(Field));
			OutPayload.Variables.Add(MoveTemp(Variable));
		}

		Props->SetNumberField(TEXT("field_count"), FieldCount);
		Props->SetArrayField(TEXT("fields"), Fields);

		OutPayload.Node.NodeName = UserDefinedStruct->GetName();
		OutPayload.Node.NodeClass = TEXT("UserDefinedStruct");
		OutPayload.Node.NodeType = TEXT("Struct");

		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutPayload.Node.Properties);
		return FJsonSerializer::Serialize(Props, *Writer, true);
	}
}

bool FUserDefinedStructIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!UserDefinedStructIndexerInternal::BuildPayload(Cast<UUserDefinedStruct>(LoadedAsset), Payload))
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

bool FUserDefinedStructIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayload(Payload, DB, AssetId);
}

bool FUserDefinedStructIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayloadToShadow(Payload, DB, AssetId, CohortName);
}
