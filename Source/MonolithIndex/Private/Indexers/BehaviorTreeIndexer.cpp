#include "Indexers/BehaviorTreeIndexer.h"

#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/*
 * 这份实现的核心目标是把旧的 sentinel 行为树索引，
 * 收口成一套真正可缓存、可 shadow、可 warmup 的 package-scoped 图 payload。
 *
 * 这里有两个很重要的取舍：
 * 1. 只保留“资产内部图结构”这类真正能参与 shadow diff 的数据；
 * 2. 不再往 connection 表里塞跨资产 class ref / asset ref 这种当前 diff 用不上的半残语义。
 *
 * 换句话说：
 * - BehaviorTree 现在关注的是“树里有哪些节点，它们怎么连”；
 * - Blackboard 现在关注的是“这个黑板有哪些键”；
 * - 两者都走同一份 graph payload 协议。
 */

namespace BehaviorTreeIndexerInternal
{
	/** 把 JSON 对象压成紧凑字符串，方便直接存进 node.properties。 */
	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	/** 往 graph payload 里追加一条 node，并返回它在数组里的下标。 */
	static int32 AddNode(
		MonolithSimpleArtifactSerialization::FGraphPayload& Payload,
		const FString& NodeType,
		const FString& NodeName,
		const FString& NodeClass,
		const FString& Properties,
		const int32 PosX = 0,
		const int32 PosY = 0)
	{
		FIndexedNode Node;
		Node.NodeType = NodeType;
		Node.NodeName = NodeName;
		Node.NodeClass = NodeClass;
		Node.Properties = Properties;
		Node.PosX = PosX;
		Node.PosY = PosY;
		return Payload.Nodes.Add(MoveTemp(Node));
	}

	/** 往 graph payload 里追加一条内部连线。 */
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

	/** 统一构建行为树节点的 JSON 属性。 */
	static bool BuildBehaviorTreeNodeProperties(
		const UBTNode* Node,
		const int32 ExecutionIndex,
		const int32 Depth,
		const int32 ChildCount,
		const int32 ServiceCount,
		const int32 DecoratorCount,
		const FString& BlackboardAssetPath,
		FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("execution_index"), ExecutionIndex);
		Properties->SetNumberField(TEXT("depth"), Depth);
		Properties->SetNumberField(TEXT("child_count"), ChildCount);
		Properties->SetNumberField(TEXT("service_count"), ServiceCount);
		Properties->SetNumberField(TEXT("decorator_count"), DecoratorCount);
		if (!BlackboardAssetPath.IsEmpty())
		{
			Properties->SetStringField(TEXT("blackboard_asset"), BlackboardAssetPath);
		}
		if (Node)
		{
			Properties->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		}
		return SerializeJsonObject(Properties, OutProperties);
	}

	/** 统一构建黑板主节点的 JSON 属性。 */
	static bool BuildBlackboardProperties(const UBlackboardData* Blackboard, const int32 DeclaredKeyCount, const int32 ResolvedKeyCount, FString& OutProperties)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("declared_key_count"), DeclaredKeyCount);
		Properties->SetNumberField(TEXT("resolved_key_count"), ResolvedKeyCount);
		Properties->SetBoolField(TEXT("has_parent"), Blackboard && Blackboard->Parent != nullptr);
		if (Blackboard && Blackboard->Parent)
		{
			Properties->SetStringField(TEXT("parent_blackboard"), Blackboard->Parent->GetPathName());
		}
		return SerializeJsonObject(Properties, OutProperties);
	}
}

bool FBehaviorTreeIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
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
	MonolithSimpleArtifactSerialization::SerializeGraphPayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FBehaviorTreeIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayload(Payload, DB, AssetId);
}

bool FBehaviorTreeIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeGraphPayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeGraphPayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FBehaviorTreeIndexer::BuildPayload(UObject* LoadedAsset, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const
{
	if (UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(LoadedAsset))
	{
		return BuildBehaviorTreePayload(BehaviorTree, OutPayload);
	}

	if (UBlackboardData* Blackboard = Cast<UBlackboardData>(LoadedAsset))
	{
		return BuildBlackboardPayload(Blackboard, OutPayload);
	}

	OutPayload = MonolithSimpleArtifactSerialization::FGraphPayload();
	return false;
}

bool FBehaviorTreeIndexer::BuildBehaviorTreePayload(UBehaviorTree* BehaviorTree, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const
{
	OutPayload = MonolithSimpleArtifactSerialization::FGraphPayload();
	if (!BehaviorTree)
	{
		return false;
	}

	UBTCompositeNode* RootNode = BehaviorTree->RootNode;
	if (!RootNode)
	{
		// 没有根节点的行为树仍然算一个“合法但为空”的快照。
		// 这样 artifact 链路不会因为空树而失败，只是最终不会写出任何 node。
		return true;
	}

	const FString BlackboardAssetPath = BehaviorTree->BlackboardAsset
		? BehaviorTree->BlackboardAsset->GetPathName()
		: FString();
	int32 ExecutionIndex = 0;

	TFunction<int32(UBTNode*, const FString&, int32, int32, int32, int32)> AddBehaviorTreeNode;
	AddBehaviorTreeNode =
		[&](UBTNode* Node, const FString& NodeType, const int32 Depth, const int32 ChildCount, const int32 ServiceCount, const int32 DecoratorCount) -> int32
	{
		if (!Node)
		{
			return INDEX_NONE;
		}

		FString Properties;
		if (!BehaviorTreeIndexerInternal::BuildBehaviorTreeNodeProperties(
			Node,
			ExecutionIndex,
			Depth,
			ChildCount,
			ServiceCount,
			DecoratorCount,
			BlackboardAssetPath,
			Properties))
		{
			return INDEX_NONE;
		}

		const int32 NodeIndex = BehaviorTreeIndexerInternal::AddNode(
			OutPayload,
			NodeType,
			Node->GetNodeName(),
			Node->GetClass()->GetName(),
			Properties);
		++ExecutionIndex;
		return NodeIndex;
	};

	TFunction<void(UBTCompositeNode*, int32, int32)> VisitComposite;
	VisitComposite = [&](UBTCompositeNode* Composite, const int32 ParentNodeIndex, const int32 Depth)
	{
		if (!Composite)
		{
			return;
		}

		const int32 CompositeNodeIndex = AddBehaviorTreeNode(
			Composite,
			TEXT("BT_Composite"),
			Depth,
			Composite->Children.Num(),
			Composite->Services.Num(),
			0);
		if (CompositeNodeIndex == INDEX_NONE)
		{
			return;
		}

		if (ParentNodeIndex != INDEX_NONE)
		{
			BehaviorTreeIndexerInternal::AddConnection(
				OutPayload,
				ParentNodeIndex,
				TEXT("Child"),
				CompositeNodeIndex,
				Composite->GetNodeName(),
				TEXT("BT_Child"));
		}

		for (UBTService* Service : Composite->Services)
		{
			const int32 ServiceNodeIndex = AddBehaviorTreeNode(Service, TEXT("BT_Service"), Depth + 1, 0, 0, 0);
			if (ServiceNodeIndex != INDEX_NONE)
			{
				BehaviorTreeIndexerInternal::AddConnection(
					OutPayload,
					CompositeNodeIndex,
					TEXT("Services"),
					ServiceNodeIndex,
					Service ? Service->GetNodeName() : TEXT("Service"),
					TEXT("BT_Service"));
			}
		}

		for (const FBTCompositeChild& Child : Composite->Children)
		{
			for (UBTDecorator* Decorator : Child.Decorators)
			{
				const int32 DecoratorNodeIndex = AddBehaviorTreeNode(Decorator, TEXT("BT_Decorator"), Depth + 1, 0, 0, 0);
				if (DecoratorNodeIndex != INDEX_NONE)
				{
					BehaviorTreeIndexerInternal::AddConnection(
						OutPayload,
						CompositeNodeIndex,
						TEXT("Decorators"),
						DecoratorNodeIndex,
						Decorator ? Decorator->GetNodeName() : TEXT("Decorator"),
						TEXT("BT_Decorator"));
				}
			}

			if (Child.ChildComposite)
			{
				VisitComposite(Child.ChildComposite, CompositeNodeIndex, Depth + 1);
				continue;
			}

			if (Child.ChildTask)
			{
				const int32 TaskNodeIndex = AddBehaviorTreeNode(
					Child.ChildTask,
					TEXT("BT_Task"),
					Depth + 1,
					0,
					Child.ChildTask->Services.Num(),
					0);
				if (TaskNodeIndex == INDEX_NONE)
				{
					continue;
				}

				BehaviorTreeIndexerInternal::AddConnection(
					OutPayload,
					CompositeNodeIndex,
					TEXT("Child"),
					TaskNodeIndex,
					Child.ChildTask->GetNodeName(),
					TEXT("BT_Child"));

				for (UBTService* TaskService : Child.ChildTask->Services)
				{
					const int32 ServiceNodeIndex = AddBehaviorTreeNode(TaskService, TEXT("BT_Service"), Depth + 2, 0, 0, 0);
					if (ServiceNodeIndex != INDEX_NONE)
					{
						BehaviorTreeIndexerInternal::AddConnection(
							OutPayload,
							TaskNodeIndex,
							TEXT("Services"),
							ServiceNodeIndex,
							TaskService ? TaskService->GetNodeName() : TEXT("Service"),
							TEXT("BT_Service"));
					}
				}
			}
		}
	};

	VisitComposite(RootNode, INDEX_NONE, 0);
	return true;
}

bool FBehaviorTreeIndexer::BuildBlackboardPayload(UBlackboardData* Blackboard, MonolithSimpleArtifactSerialization::FGraphPayload& OutPayload) const
{
	OutPayload = MonolithSimpleArtifactSerialization::FGraphPayload();
	if (!Blackboard)
	{
		return false;
	}

	TArray<const UBlackboardData*> BlackboardChain;
	for (const UBlackboardData* Current = Blackboard; Current; Current = Current->Parent)
	{
		// 从根到叶排好顺序，确保变量顺序稳定。
		BlackboardChain.Insert(Current, 0);
	}

	int32 ResolvedKeyCount = 0;
	for (const UBlackboardData* BlackboardLayer : BlackboardChain)
	{
		ResolvedKeyCount += BlackboardLayer ? BlackboardLayer->Keys.Num() : 0;
	}

	FString Properties;
	if (!BehaviorTreeIndexerInternal::BuildBlackboardProperties(Blackboard, Blackboard->Keys.Num(), ResolvedKeyCount, Properties))
	{
		return false;
	}

	BehaviorTreeIndexerInternal::AddNode(
		OutPayload,
		TEXT("Blackboard"),
		Blackboard->GetName(),
		Blackboard->GetClass()->GetName(),
		Properties);

	for (const UBlackboardData* BlackboardLayer : BlackboardChain)
	{
		if (!BlackboardLayer)
		{
			continue;
		}

		for (const FBlackboardEntry& Key : BlackboardLayer->Keys)
		{
			FIndexedVariable Variable;
			Variable.VarName = Key.EntryName.ToString();
			Variable.VarType = Key.KeyType ? Key.KeyType->GetClass()->GetName() : TEXT("Unknown");
			Variable.Category = TEXT("Blackboard");
			Variable.DefaultValue = BlackboardLayer->GetName();
			Variable.bIsExposed = false;
			Variable.bIsReplicated = Key.bInstanceSynced;
			OutPayload.Variables.Add(MoveTemp(Variable));
		}
	}

	return true;
}
