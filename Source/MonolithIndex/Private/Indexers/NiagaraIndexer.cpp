#include "Indexers/NiagaraIndexer.h"

#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * Niagara 这条链路现在和 Animation / InputAction 很像：
 * - 先把运行时对象压成轻量 payload；
 * - 再把 payload 写到正式表或 shadow 表；
 * - warmup 则只负责提前构建 payload 并塞进缓存。
 *
 * 这样做的好处是：
 * - full / incremental / live / warmup / shadow 都共用同一份“怎么抽取 Niagara 摘要”的逻辑；
 * - 去掉旧 sentinel 后，不会再出现“同一类数据两套入口”的维护分叉。
 */

namespace NiagaraIndexerInternal
{
	/** 把 JSON 对象压成紧凑字符串，方便直接塞进 node.properties。 */
	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
}

bool FNiagaraIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;
	MonolithSimpleArtifactSerialization::FNodesPayload Payload;
	if (!BuildPayload(Cast<UNiagaraSystem>(LoadedAsset), Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeNodesPayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FNiagaraIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodesPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodesPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodesPayload(Payload, DB, AssetId);
}

bool FNiagaraIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodesPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodesPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodesPayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FNiagaraIndexer::BuildPayload(UNiagaraSystem* System, MonolithSimpleArtifactSerialization::FNodesPayload& OutPayload) const
{
	OutPayload = MonolithSimpleArtifactSerialization::FNodesPayload();
	if (!System)
	{
		return false;
	}

	FIndexedNode SystemNode;
	if (!BuildSystemNode(System, SystemNode))
	{
		return false;
	}

	OutPayload.Nodes.Add(MoveTemp(SystemNode));
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FIndexedNode EmitterNode;
		if (BuildEmitterNode(Handle, EmitterNode))
		{
			OutPayload.Nodes.Add(MoveTemp(EmitterNode));
		}
	}

	return OutPayload.Nodes.Num() > 0;
}

bool FNiagaraIndexer::BuildSystemNode(UNiagaraSystem* System, FIndexedNode& OutNode) const
{
	if (!System)
	{
		return false;
	}

	auto SystemProps = MakeShared<FJsonObject>();
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	SystemProps->SetBoolField(TEXT("has_fixed_bounds"), System->bFixedBounds);
	if (System->bFixedBounds)
	{
		const FBox& Bounds = System->GetFixedBounds();
		SystemProps->SetStringField(TEXT("fixed_bounds_min"), Bounds.Min.ToString());
		SystemProps->SetStringField(TEXT("fixed_bounds_max"), Bounds.Max.ToString());
	}
	SystemProps->SetNumberField(TEXT("emitter_count"), EmitterHandles.Num());
	SystemProps->SetBoolField(TEXT("is_valid"), System->IsValid());

	TArray<TSharedPtr<FJsonValue>> EmitterNames;
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		EmitterNames.Add(MakeShared<FJsonValueString>(Handle.GetName().ToString()));
	}
	SystemProps->SetArrayField(TEXT("emitter_names"), EmitterNames);

	OutNode = FIndexedNode();
	OutNode.NodeName = System->GetName();
	OutNode.NodeClass = TEXT("NiagaraSystem");
	OutNode.NodeType = TEXT("System");
	return NiagaraIndexerInternal::SerializeJsonObject(SystemProps, OutNode.Properties);
}

bool FNiagaraIndexer::BuildEmitterNode(const FNiagaraEmitterHandle& Handle, FIndexedNode& OutNode) const
{
	auto EmitterProps = MakeShared<FJsonObject>();
	EmitterProps->SetStringField(TEXT("name"), Handle.GetName().ToString());
	EmitterProps->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

	FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
	if (VersionedEmitter.Emitter)
	{
		if (FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.GetEmitterData())
		{
			switch (EmitterData->SimTarget)
			{
			case ENiagaraSimTarget::CPUSim:
				EmitterProps->SetStringField(TEXT("sim_target"), TEXT("CPU"));
				break;
			case ENiagaraSimTarget::GPUComputeSim:
				EmitterProps->SetStringField(TEXT("sim_target"), TEXT("GPU"));
				break;
			default:
				EmitterProps->SetStringField(TEXT("sim_target"), TEXT("Unknown"));
				break;
			}

			TArray<TSharedPtr<FJsonValue>> RendererArray;
			for (const UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				if (Renderer)
				{
					RendererArray.Add(MakeShared<FJsonValueString>(Renderer->GetClass()->GetName()));
				}
			}
			EmitterProps->SetArrayField(TEXT("renderers"), RendererArray);

			EmitterProps->SetBoolField(TEXT("has_spawn_script"), EmitterData->SpawnScriptProps.Script != nullptr);
			EmitterProps->SetBoolField(TEXT("has_update_script"), EmitterData->UpdateScriptProps.Script != nullptr);
		}
	}

	OutNode = FIndexedNode();
	OutNode.NodeName = Handle.GetName().ToString();
	OutNode.NodeClass = TEXT("NiagaraEmitter");
	OutNode.NodeType = TEXT("Emitter");
	return NiagaraIndexerInternal::SerializeJsonObject(EmitterProps, OutNode.Properties);
}
