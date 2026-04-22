#include "Indexers/InputActionIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * InputAction 的 artifact 很轻：
 * 本质上就是把一小份配置摘要压成单 node。
 *
 * 这样 warmup 和 shadow mode 都能很快复用，不需要每次重新加载并手工遍历。
 */

namespace InputActionIndexerInternal
{
	static bool BuildPayload(UInputAction* InputAction, MonolithSimpleArtifactSerialization::FNodePayload& OutPayload)
	{
		if (!InputAction)
		{
			return false;
		}

		auto Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("value_type"), UEnum::GetValueAsString(InputAction->ValueType));

		if (!InputAction->ActionDescription.IsEmpty())
		{
			Props->SetStringField(TEXT("description"), InputAction->ActionDescription.ToString());
		}

		Props->SetBoolField(TEXT("consume_input"), InputAction->bConsumeInput);
		Props->SetBoolField(TEXT("trigger_when_paused"), InputAction->bTriggerWhenPaused);

		TArray<TSharedPtr<FJsonValue>> TriggerArray;
		for (const UInputTrigger* Trigger : InputAction->Triggers)
		{
			// 这里先记录类名摘要。
			// 如果以后要做更细的 Trigger 参数比较，再扩展 payload 即可。
			if (Trigger)
			{
				TriggerArray.Add(MakeShared<FJsonValueString>(Trigger->GetClass()->GetName()));
			}
		}
		Props->SetArrayField(TEXT("triggers"), TriggerArray);

		TArray<TSharedPtr<FJsonValue>> ModifierArray;
		for (const UInputModifier* Modifier : InputAction->Modifiers)
		{
			if (Modifier)
			{
				ModifierArray.Add(MakeShared<FJsonValueString>(Modifier->GetClass()->GetName()));
			}
		}
		Props->SetArrayField(TEXT("modifiers"), ModifierArray);

		OutPayload = MonolithSimpleArtifactSerialization::FNodePayload();
		OutPayload.Node.NodeName = InputAction->GetName();
		OutPayload.Node.NodeClass = TEXT("InputAction");
		OutPayload.Node.NodeType = TEXT("InputAction");

		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutPayload.Node.Properties);
		return FJsonSerializer::Serialize(Props, *Writer, true);
	}
}

bool FInputActionIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!InputActionIndexerInternal::BuildPayload(Cast<UInputAction>(LoadedAsset), Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeNodePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FInputActionIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayload(Payload, DB, AssetId);
}

bool FInputActionIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayloadToShadow(Payload, DB, AssetId, CohortName);
}
