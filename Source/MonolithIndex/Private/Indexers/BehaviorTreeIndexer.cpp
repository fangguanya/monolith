#include "Indexers/BehaviorTreeIndexer.h"
#include "MonolithSettings.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

static bool SafeCallWithSEH_BT(void(*Func)(void*), void* Context)
{
	__try
	{
		Func(Context);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
#endif

// SEH 安全调用上下文
struct FBTIndexCallContext
{
	FBehaviorTreeIndexer* Self;
	FMonolithIndexDatabase* DB;
	int64 AssetId;
	FSoftObjectPath ObjectPath;
	bool bSuccess;
	enum EType { BehaviorTree, Blackboard } Type;
};

static void LoadAndIndexBTAsset(void* Ctx)
{
	auto* C = static_cast<FBTIndexCallContext*>(Ctx);
	C->bSuccess = false;

	UObject* Loaded = C->ObjectPath.TryLoad();
	if (!Loaded) return;

	switch (C->Type)
	{
	case FBTIndexCallContext::BehaviorTree:
		if (UBehaviorTree* BT = Cast<UBehaviorTree>(Loaded))
		{ C->Self->IndexBehaviorTreePublic(BT, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	case FBTIndexCallContext::Blackboard:
		if (UBlackboardData* BB = Cast<UBlackboardData>(Loaded))
		{ C->Self->IndexBlackboardPublic(BB, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	}
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

FString FBehaviorTreeIndexer::JsonToString(TSharedPtr<FJsonObject> JsonObj)
{
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Out;
}

// ─────────────────────────────────────────────────────────────
// Sentinel 入口
// ─────────────────────────────────────────────────────────────

bool FBehaviorTreeIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	int32 TotalIndexed = 0;

	// 按资产类型枚举并索引
	auto IndexAllOfType = [&](UClass* Class, FBTIndexCallContext::EType Type) -> int32
	{
		TArray<FAssetData> Assets;
		FARFilter Filter;
		for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
		{
			Filter.PackagePaths.Add(ContentPath);
		}
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Registry.GetAssets(Filter, Assets);

		int32 Count = 0;
		for (const FAssetData& AD : Assets)
		{
			int64 AId = DB.GetAssetId(AD.PackageName.ToString());
			if (AId < 0) continue;

			FBTIndexCallContext Ctx;
			Ctx.Self = this;
			Ctx.DB = &DB;
			Ctx.AssetId = AId;
			Ctx.ObjectPath = AD.GetSoftObjectPath();
			Ctx.bSuccess = false;
			Ctx.Type = Type;

#if PLATFORM_WINDOWS
			if (SafeCallWithSEH_BT(&LoadAndIndexBTAsset, &Ctx))
			{
				if (Ctx.bSuccess) Count++;
			}
			else
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("BehaviorTreeIndexer: crashed loading/indexing '%s' - skipping"), *AD.GetSoftObjectPath().ToString());
			}
#else
			LoadAndIndexBTAsset(&Ctx);
			if (Ctx.bSuccess) Count++;
#endif
		}
		return Count;
	};

	TotalIndexed += IndexAllOfType(UBehaviorTree::StaticClass(), FBTIndexCallContext::BehaviorTree);
	TotalIndexed += IndexAllOfType(UBlackboardData::StaticClass(), FBTIndexCallContext::Blackboard);

	UE_LOG(LogMonolithIndex, Log, TEXT("BehaviorTreeIndexer: indexed %d BT/Blackboard assets"), TotalIndexed);
	return true;
}

// ─────────────────────────────────────────────────────────────
// BehaviorTree 节点遍历
// ─────────────────────────────────────────────────────────────

void FBehaviorTreeIndexer::IndexBehaviorTree(UBehaviorTree* BT, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!BT) return;

	// BT→Blackboard 交叉引用
	if (BT->BlackboardAsset)
	{
		int64 BBAssetId = DB.GetAssetId(BT->BlackboardAsset->GetPackage()->GetName());
		if (BBAssetId >= 0)
		{
			FIndexedConnection Conn;
			Conn.SourceNodeId = AssetId;
			Conn.TargetNodeId = BBAssetId;
			Conn.PinType = TEXT("BT_UsesBlackboard");
			DB.InsertConnection(Conn);
		}
	}

	UBTCompositeNode* RootNode = BT->RootNode;
	if (!RootNode) return;

	// 收集唯一节点类用于交叉引用
	TSet<FString> UniqueNodeClasses;

	// 节点执行索引计数器
	int32 ExecutionIndex = 0;

	// 辅助 lambda：索引单个 BTNode
	auto IndexSingleNode = [&](UBTNode* Node, const FString& NodeType, int32 ParentExecIndex, int32 Depth)
	{
		if (!Node) return;

		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("execution_index"), ExecutionIndex);
		Props->SetNumberField(TEXT("parent_execution_index"), ParentExecIndex);
		Props->SetNumberField(TEXT("depth"), Depth);

		const FString ClassName = Node->GetClass()->GetName();
		UniqueNodeClasses.Add(ClassName);

		FIndexedNode IndexedNode;
		IndexedNode.AssetId = AssetId;
		IndexedNode.NodeType = NodeType;
		IndexedNode.NodeName = Node->GetNodeName();
		IndexedNode.NodeClass = ClassName;
		IndexedNode.Properties = JsonToString(Props);
		DB.InsertNode(IndexedNode);

		ExecutionIndex++;
	};

	// DFS 遍历 BT 节点树
	struct FStackEntry
	{
		UBTCompositeNode* Composite;
		int32 ParentExecIndex;
		int32 Depth;
	};

	TArray<FStackEntry> Stack;
	Stack.Add({ RootNode, -1, 0 });

	while (Stack.Num() > 0)
	{
		FStackEntry Current = Stack.Pop();
		UBTCompositeNode* Composite = Current.Composite;
		if (!Composite) continue;

		int32 CompositeExecIdx = ExecutionIndex;
		IndexSingleNode(Composite, TEXT("BT_Composite"), Current.ParentExecIndex, Current.Depth);

		// Composite 上的 Services
		for (UBTService* Service : Composite->Services)
		{
			IndexSingleNode(Service, TEXT("BT_Service"), CompositeExecIdx, Current.Depth + 1);
		}

		// 遍历 Children（FBTCompositeChild）
		for (int32 i = 0; i < Composite->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Composite->Children[i];

			// Child 上的 Decorators
			for (UBTDecorator* Dec : Child.Decorators)
			{
				IndexSingleNode(Dec, TEXT("BT_Decorator"), CompositeExecIdx, Current.Depth + 1);
			}

			if (Child.ChildComposite)
			{
				// 子 Composite 节点入栈
				Stack.Add({ Child.ChildComposite, CompositeExecIdx, Current.Depth + 1 });
			}
			else if (Child.ChildTask)
			{
				// Task 节点
				int32 TaskExecIdx = ExecutionIndex;
				IndexSingleNode(Child.ChildTask, TEXT("BT_Task"), CompositeExecIdx, Current.Depth + 1);

				// Task 上的 Services
				for (UBTService* TaskSvc : Child.ChildTask->Services)
				{
					IndexSingleNode(TaskSvc, TEXT("BT_Service"), TaskExecIdx, Current.Depth + 2);
				}
			}
		}
	}

	// BT→NodeClass 交叉引用（每个唯一类一条）
	for (const FString& ClassName : UniqueNodeClasses)
	{
		FIndexedConnection Conn;
		Conn.SourceNodeId = AssetId;
		Conn.SourcePin = BT->GetName();
		Conn.TargetPin = ClassName;
		Conn.PinType = TEXT("BT_UsesNodeClass");
		DB.InsertConnection(Conn);
	}
}

// ─────────────────────────────────────────────────────────────
// Blackboard 键索引
// ─────────────────────────────────────────────────────────────

void FBehaviorTreeIndexer::IndexBlackboard(UBlackboardData* BB, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!BB) return;

	// 包括父 Blackboard 的键
	TArray<const UBlackboardData*> BBChain;
	const UBlackboardData* Current = BB;
	while (Current)
	{
		BBChain.Insert(Current, 0);
		Current = Current->Parent;
	}

	for (const UBlackboardData* BBData : BBChain)
	{
		for (const FBlackboardEntry& Key : BBData->Keys)
		{
			FIndexedVariable Var;
			Var.AssetId = AssetId;
			Var.VarName = Key.EntryName.ToString();
			Var.VarType = Key.KeyType ? Key.KeyType->GetClass()->GetName() : TEXT("Unknown");
			Var.Category = TEXT("Blackboard");
			Var.bIsReplicated = Key.bInstanceSynced;
			DB.InsertVariable(Var);
		}
	}

	// 如果有父 Blackboard，建立引用关系
	if (BB->Parent)
	{
		int64 ParentAssetId = DB.GetAssetId(BB->Parent->GetPackage()->GetName());
		if (ParentAssetId >= 0)
		{
			FIndexedConnection Conn;
			Conn.SourceNodeId = AssetId;
			Conn.TargetNodeId = ParentAssetId;
			Conn.PinType = TEXT("BB_InheritsFrom");
			DB.InsertConnection(Conn);
		}
	}
}
