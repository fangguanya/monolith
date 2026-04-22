#include "Indexers/DataTableIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * DataTable 的实现重点是两件事：
 * 1. 把每一行稳定地转成 JSON；
 * 2. 保证行顺序稳定，这样 artifact 和 shadow diff 才不会因为遍历顺序不同而抖动。
 */

bool FDataTableIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;
	MonolithSimpleArtifactSerialization::FDataTablePayload Payload;
	if (!BuildPayload(Cast<UDataTable>(LoadedAsset), Payload.Rows))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeDataTablePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FDataTableIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FDataTablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeDataTablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeDataTablePayload(Payload, DB, AssetId);
}

bool FDataTableIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FDataTablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeDataTablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeDataTablePayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FDataTableIndexer::BuildPayload(UDataTable* DataTable, TArray<FIndexedDataTableRow>& OutRows) const
{
	OutRows.Reset();

	if (!DataTable)
	{
		return false;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return false;
	}

	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
	OutRows.Reserve(RowMap.Num());

	for (const TPair<FName, uint8*>& Pair : RowMap)
	{
		// 每一行都会变成一条独立记录，方便后面做按行比较和回放。
		FIndexedDataTableRow IndexedRow;
		IndexedRow.RowName = Pair.Key.ToString();
		IndexedRow.RowData = RowStructToJson(RowStruct, Pair.Value);
		OutRows.Add(MoveTemp(IndexedRow));
	}

	OutRows.Sort([](const FIndexedDataTableRow& A, const FIndexedDataTableRow& B)
	{
		// 明确按行名排序，避免 Map 遍历顺序不稳定带来伪 diff。
		return A.RowName < B.RowName;
	});

	return true;
}

FString FDataTableIndexer::RowStructToJson(const UScriptStruct* RowStruct, const void* RowData)
{
	auto JsonObj = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		const FString PropName = Prop->GetName();
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (NumProp->IsInteger())
			{
				JsonObj->SetNumberField(PropName, static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
			}
			else if (NumProp->IsFloatingPoint())
			{
				JsonObj->SetNumberField(PropName, NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
		}
		else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			JsonObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			JsonObj->SetStringField(PropName, TextProp->GetPropertyValue(ValuePtr).ToString());
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			FString EnumValue;
			Prop->ExportTextItem_Direct(EnumValue, ValuePtr, nullptr, nullptr, PPF_None);
			JsonObj->SetStringField(PropName, EnumValue);
		}
		else if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr& SoftPtr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
			JsonObj->SetStringField(PropName, SoftPtr.ToSoftObjectPath().ToString());
		}
		else
		{
			// struct、array 这类复杂字段先退回文本导出，
			// 至少保证信息不会丢，只是粒度会比基础类型粗一点。
			FString ExportedValue;
			Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
			JsonObj->SetStringField(PropName, ExportedValue);
		}
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Result;
}
