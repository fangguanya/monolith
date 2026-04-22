#if WITH_STATETREE

#include "Indexers/StateTreeIndexer.h"

#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StateTree.h"
#include "StateTreeNodeBase.h"
#include "StateTreeTaskBase.h"

/*
 * StateTree 这条链路的重点是“状态机本体”：
 * - 有哪些状态；
 * - 每个状态挂了哪些任务；
 * - 状态之间怎么跳。
 *
 * 旧实现把任务类名塞回状态属性里，外加一批 class-ref 连接。
 * 这次改成 graph payload 后，任务本身也会变成节点，
 * 这样 diff 才能真正比较状态图结构。
 */

namespace StateTreeIndexerInternal
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

	/** 构建状态节点属性。 */
	static bool BuildStateProperties(
		const FCompactStateTreeState& State,
		const TConstArrayView<FCompactStateTreeState>& States,
		FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(
			TEXT("state_type"),
			StaticEnum<EStateTreeStateType>()->GetNameStringByValue(static_cast<int64>(State.Type)));
		Properties->SetNumberField(TEXT("num_children"), State.HasChildren() ? (State.ChildrenEnd - State.ChildrenBegin) : 0);
		Properties->SetNumberField(TEXT("num_transitions"), State.TransitionsNum);
		Properties->SetNumberField(TEXT("num_tasks"), State.TasksNum);
		if (State.Parent.IsValid() && State.Parent.Index < States.Num())
		{
			Properties->SetStringField(TEXT("parent_state"), States[State.Parent.Index].Name.ToString());
		}
		return SerializeJsonObject(Properties, OutProperties);
	}

	/** 构建任务节点属性。 */
	static bool BuildTaskProperties(const FString& StateName, const int32 TaskIndex, const UScriptStruct* ScriptStruct, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("state_name"), StateName);
		Properties->SetNumberField(TEXT("task_index"), TaskIndex);
		Properties->SetStringField(TEXT("class"), ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"));
		return SerializeJsonObject(Properties, OutProperties);
	}
}

bool FStateTreeIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!BuildPayload(Cast<UStateTree>(LoadedAsset), Payload))
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

bool FStateTreeIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayload(Payload, DB, AssetId);
}

bool FStateTreeIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FStateTreeIndexer::BuildPayload(UStateTree* StateTree, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const
{
	OutPayload = MonolithSimpleArtifactSerialization::FGraphPayload();
	if (!StateTree || !StateTree->IsReadyToRun())
	{
		return false;
	}

	const TConstArrayView<FCompactStateTreeState> States = StateTree->GetStates();
	TMap<int32, int32> StateIndexToNodeIndex;

	// 第一步：先把所有 State 都变成节点。
	// 这样后面建立 transition 时，就能稳定地拿到源/目标节点下标。
	for (int32 StateIndex = 0; StateIndex < States.Num(); ++StateIndex)
	{
		const FCompactStateTreeState& State = States[StateIndex];

		FString StateProperties;
		if (!StateTreeIndexerInternal::BuildStateProperties(State, States, StateProperties))
		{
			return false;
		}

		const int32 StateNodeIndex = StateTreeIndexerInternal::AddNode(
			OutPayload,
			TEXT("ST_State"),
			State.Name.ToString(),
			TEXT("FCompactStateTreeState"),
			StateProperties);
		StateIndexToNodeIndex.Add(StateIndex, StateNodeIndex);
	}

	// 第二步：把每个 State 挂着的 Task 也变成节点，并连回自己的 State。
	for (int32 StateIndex = 0; StateIndex < States.Num(); ++StateIndex)
	{
		const FCompactStateTreeState& State = States[StateIndex];
		const int32* StateNodeIndex = StateIndexToNodeIndex.Find(StateIndex);
		if (!StateNodeIndex)
		{
			return false;
		}

		for (uint16 TaskOffset = 0; TaskOffset < State.TasksNum; ++TaskOffset)
		{
			const int32 NodeIndex = State.TasksBegin + TaskOffset;
			const FConstStructView NodeView = StateTree->GetNode(NodeIndex);
			if (!NodeView.IsValid())
			{
				continue;
			}

			const UScriptStruct* ScriptStruct = NodeView.GetScriptStruct();
			FString TaskProperties;
			if (!StateTreeIndexerInternal::BuildTaskProperties(State.Name.ToString(), TaskOffset, ScriptStruct, TaskProperties))
			{
				return false;
			}

			const int32 TaskNodeIndex = StateTreeIndexerInternal::AddNode(
				OutPayload,
				TEXT("ST_Task"),
				FString::Printf(TEXT("%s.Task.%d"), *State.Name.ToString(), TaskOffset),
				ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"),
				TaskProperties);
			StateTreeIndexerInternal::AddConnection(
				OutPayload,
				*StateNodeIndex,
				TEXT("Tasks"),
				TaskNodeIndex,
				ScriptStruct ? ScriptStruct->GetName() : TEXT("Task"),
				TEXT("ST_Task"));
		}
	}

	// 第三步：建立 State -> State 的 transition 连接。
	for (int32 StateIndex = 0; StateIndex < States.Num(); ++StateIndex)
	{
		const FCompactStateTreeState& State = States[StateIndex];
		const int32* SourceNodeIndex = StateIndexToNodeIndex.Find(StateIndex);
		if (!SourceNodeIndex)
		{
			return false;
		}

		for (uint8 TransitionOffset = 0; TransitionOffset < State.TransitionsNum; ++TransitionOffset)
		{
			const FStateTreeIndex16 TransitionIndex(State.TransitionsBegin + TransitionOffset);
			const FCompactStateTransition* Transition = StateTree->GetTransitionFromIndex(TransitionIndex);
			if (!Transition || !Transition->State.IsValid())
			{
				continue;
			}

			const int32* TargetNodeIndex = StateIndexToNodeIndex.Find(Transition->State.Index);
			if (!TargetNodeIndex)
			{
				continue;
			}

			StateTreeIndexerInternal::AddConnection(
				OutPayload,
				*SourceNodeIndex,
				States[StateIndex].Name.ToString(),
				*TargetNodeIndex,
				States[Transition->State.Index].Name.ToString(),
				TEXT("ST_Transition"));
		}
	}

	return true;
}

#endif // WITH_STATETREE
