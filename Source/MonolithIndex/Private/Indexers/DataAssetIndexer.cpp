#include "Indexers/DataAssetIndexer.h"
#include "Engine/DataAsset.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * DataAssetIndexer 的目标很直接：
 * 把“主要靠 UPROPERTY 配出来的资产”翻译成 Monolith 能搜索、能 diff 的统一结构。
 *
 * 这里我们会生成两层结果：
 * 1. 一个 node：保存整份属性树的大 JSON 快照；
 * 2. 一组 variables：把每个重要属性拆成独立行，方便全文搜索和 shadow diff。
 */

bool FDataAssetIndexer::ShouldSkipNodeProperty(const FProperty* Prop)
{
	return !Prop
		|| Prop->GetOwnerClass() == UObject::StaticClass()
		|| Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient);
}

bool FDataAssetIndexer::ShouldSkipVariableProperty(const FProperty* Prop)
{
	return ShouldSkipNodeProperty(Prop)
		|| Prop->GetOwnerClass() == UDataAsset::StaticClass();
}

bool FDataAssetIndexer::BuildPayload(UObject* Object, MonolithSimpleArtifactSerialization::FNodeVariablePayload& OutPayload)
{
	if (!Object)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Props = SerializeObjectProperties(Object);
	if (!Props)
	{
		return false;
	}

	OutPayload = MonolithSimpleArtifactSerialization::FNodeVariablePayload();
	OutPayload.Node.NodeType = TEXT("DataAsset");
	OutPayload.Node.NodeName = Object->GetName();
	OutPayload.Node.NodeClass = Object->GetClass()->GetName();

	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutPayload.Node.Properties);
	if (!FJsonSerializer::Serialize(Props, *Writer, true))
	{
		return false;
	}

	UClass* ObjClass = Object->GetClass();
	for (TFieldIterator<FProperty> It(ObjClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (ShouldSkipVariableProperty(Prop))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);

		FString DefaultStr;
		Prop->ExportTextItem_Direct(DefaultStr, ValuePtr, nullptr, Object, PPF_None);

		// 空值、None 和空括号通常没什么搜索意义，跳过能让结果更干净。
		if (DefaultStr.IsEmpty() || DefaultStr == TEXT("None") || DefaultStr == TEXT("()"))
		{
			continue;
		}

		FIndexedVariable Var;
		Var.VarName = Prop->GetName();
		Var.VarType = Prop->GetCPPType();
		Var.Category = Prop->GetMetaData(TEXT("Category"));
		Var.DefaultValue = DefaultStr;
		Var.bIsExposed = false;
		Var.bIsReplicated = !!(Prop->PropertyFlags & CPF_Net);
		OutPayload.Variables.Add(MoveTemp(Var));
	}

	return true;
}

bool FDataAssetIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!BuildPayload(LoadedAsset, Payload))
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

bool FDataAssetIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayload(Payload, DB, AssetId);
}

bool FDataAssetIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodeVariablePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodeVariablePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodeVariablePayloadToShadow(Payload, DB, AssetId, CohortName);
}

TSharedPtr<FJsonObject> FDataAssetIndexer::SerializeObjectProperties(UObject* Object)
{
	if (!Object)
	{
		return nullptr;
	}

	auto Root = MakeShared<FJsonObject>();
	UClass* ObjClass = Object->GetClass();

	Root->SetStringField(TEXT("native_class"), ObjClass->GetName());

	auto PropsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(ObjClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FProperty* Prop = *It;
		if (ShouldSkipNodeProperty(Prop))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
		PropsObj->SetField(Prop->GetName(), PropertyToJsonValue(Prop, ValuePtr, Object));
	}

	Root->SetObjectField(TEXT("properties"), PropsObj);
	return Root;
}

TSharedPtr<FJsonValue> FDataAssetIndexer::PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, const UObject* Owner)
{
	if (!Prop || !ValuePtr)
	{
		return MakeShared<FJsonValueNull>();
	}

	if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				uint8 Val = ByteProp->GetPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(ByteProp->Enum->GetNameStringByValue(Val));
			}
		}
		if (NumProp->IsInteger())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}
		if (NumProp->IsFloatingPoint())
		{
			return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
		}
	}

	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
	}

	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FString Val;
		EnumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Val);
	}

	if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
	}
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
	}
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
	}

	if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		return MakeShared<FJsonValueString>(Ref.ToSoftObjectPath().ToString());
	}
	if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		return MakeShared<FJsonValueString>(Ref.ToSoftObjectPath().ToString());
	}
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UClass* Val = Cast<UClass>(ClassProp->GetObjectPropertyValue(ValuePtr));
		return MakeShared<FJsonValueString>(Val ? Val->GetPathName() : TEXT("None"));
	}
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		UObject* Val = ObjProp->GetObjectPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(Val ? Val->GetPathName() : TEXT("None"));
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		auto StructObj = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			FProperty* Inner = *It;
			const void* InnerPtr = Inner->ContainerPtrToValuePtr<void>(ValuePtr);
			StructObj->SetField(Inner->GetName(), PropertyToJsonValue(Inner, InnerPtr, Owner));
		}
		return MakeShared<FJsonValueObject>(StructObj);
	}

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			Arr.Add(PropertyToJsonValue(ArrayProp->Inner, Helper.GetRawPtr(i), Owner));
		}
		return MakeShared<FJsonValueArray>(Arr);
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		FScriptSetHelper Helper(SetProp, ValuePtr);
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			if (Helper.IsValidIndex(i))
			{
				Arr.Add(PropertyToJsonValue(SetProp->ElementProp, Helper.GetElementPtr(i), Owner));
			}
		}
		return MakeShared<FJsonValueArray>(Arr);
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		auto MapObj = MakeShared<FJsonObject>();
		FScriptMapHelper Helper(MapProp, ValuePtr);
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			if (Helper.IsValidIndex(i))
			{
				FString KeyStr;
				MapProp->KeyProp->ExportTextItem_Direct(KeyStr, Helper.GetKeyPtr(i), nullptr, nullptr, PPF_None);
				MapObj->SetField(KeyStr, PropertyToJsonValue(MapProp->ValueProp, Helper.GetValuePtr(i), Owner));
			}
		}
		return MakeShared<FJsonValueObject>(MapObj);
	}

	FString ExportedStr;
	Prop->ExportTextItem_Direct(ExportedStr, ValuePtr, nullptr, const_cast<UObject*>(Owner), PPF_None);
	return MakeShared<FJsonValueString>(ExportedStr);
}
