#if WITH_STATETREE

#include "Indexers/StateTreeIndexer.h"
#include "MonolithSettings.h"
#include "StateTree.h"
#include "StateTreeTaskBase.h"
#include "StateTreeNodeBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

static bool SafeCallWithSEH_ST(void(*Func)(void*), void* Context)
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
struct FSTIndexCallContext
{
	FStateTreeIndexer* Self;
	FMonolithIndexDatabase* DB;
	int64 AssetId;
	FSoftObjectPath ObjectPath;
	bool bSuccess;
};

static void LoadAndIndexSTAsset(void* Ctx)
{
	auto* C = static_cast<FSTIndexCallContext*>(Ctx);
	C->bSuccess = false;

	UObject* Loaded = C->ObjectPath.TryLoad();
	if (!Loaded) return;

	if (UStateTree* ST = Cast<UStateTree>(Loaded))
	{
		C->Self->IndexStateTreePublic(ST, *C->DB, C->AssetId);
		C->bSuccess = true;
	}
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

FString FStateTreeIndexer::JsonToString(TSharedPtr<FJsonObject> JsonObj)
{
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Out;
}

// ─────────────────────────────────────────────────────────────
// Sentinel 入口
// ─────────────────────────────────────────────────────────────

bool FStateTreeIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, Assets);

	int32 TotalIndexed = 0;
	for (const FAssetData& AD : Assets)
	{
		int64 AId = DB.GetAssetId(AD.PackageName.ToString());
		if (AId < 0) continue;

		FSTIndexCallContext Ctx;
		Ctx.Self = this;
		Ctx.DB = &DB;
		Ctx.AssetId = AId;
		Ctx.ObjectPath = AD.GetSoftObjectPath();
		Ctx.bSuccess = false;

#if PLATFORM_WINDOWS
		if (SafeCallWithSEH_ST(&LoadAndIndexSTAsset, &Ctx))
		{
			if (Ctx.bSuccess) TotalIndexed++;
		}
		else
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("StateTreeIndexer: crashed loading/indexing '%s' - skipping"), *AD.GetSoftObjectPath().ToString());
		}
#else
		LoadAndIndexSTAsset(&Ctx);
		if (Ctx.bSuccess) TotalIndexed++;
#endif
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("StateTreeIndexer: indexed %d StateTree assets"), TotalIndexed);
	return true;
}

// ─────────────────────────────────────────────────────────────
// StateTree 索引
// ─────────────────────────────────────────────────────────────

void FStateTreeIndexer::IndexStateTree(UStateTree* ST, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!ST || !ST->IsReadyToRun()) return;

	TConstArrayView<FCompactStateTreeState> States = ST->GetStates();
	const FInstancedStructContainer& Nodes = ST->GetNodes();

	// 收集唯一任务类名用于交叉引用
	TSet<FString> UniqueTaskClasses;

	// 记录 state index → InsertNode 返回的 NodeId，用于转换连接
	TMap<int32, int64> StateIndexToNodeId;

	// 索引每个 State
	for (int32 StateIdx = 0; StateIdx < States.Num(); ++StateIdx)
	{
		const FCompactStateTreeState& State = States[StateIdx];

		auto Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("state_type"), StaticEnum<EStateTreeStateType>()->GetNameStringByValue(static_cast<int64>(State.Type)));
		Props->SetNumberField(TEXT("num_children"), State.HasChildren() ? (State.ChildrenEnd - State.ChildrenBegin) : 0);
		Props->SetNumberField(TEXT("num_transitions"), State.TransitionsNum);
		Props->SetNumberField(TEXT("num_tasks"), State.TasksNum);

		// 父状态名
		if (State.Parent.IsValid() && State.Parent.Index < States.Num())
		{
			Props->SetStringField(TEXT("parent_state"), States[State.Parent.Index].Name.ToString());
		}

		// 收集该 State 的任务类名
		TArray<TSharedPtr<FJsonValue>> TasksArr;
		for (uint16 TaskOffset = 0; TaskOffset < State.TasksNum; ++TaskOffset)
		{
			int32 NodeIdx = State.TasksBegin + TaskOffset;
			FConstStructView NodeView = ST->GetNode(NodeIdx);
			if (NodeView.IsValid())
			{
				const UScriptStruct* ScriptStruct = NodeView.GetScriptStruct();
				if (ScriptStruct)
				{
					const FString TaskClassName = ScriptStruct->GetName();
					TasksArr.Add(MakeShared<FJsonValueString>(TaskClassName));
					UniqueTaskClasses.Add(TaskClassName);
				}
			}
		}
		Props->SetArrayField(TEXT("tasks"), TasksArr);

		FIndexedNode IndexedNode;
		IndexedNode.AssetId = AssetId;
		IndexedNode.NodeType = TEXT("ST_State");
		IndexedNode.NodeName = State.Name.ToString();
		IndexedNode.NodeClass = TEXT("FCompactStateTreeState");
		IndexedNode.Properties = JsonToString(Props);
		int64 NodeId = DB.InsertNode(IndexedNode);
		StateIndexToNodeId.Add(StateIdx, NodeId);
	}

	// 索引 Transitions（State → State 连接）
	for (int32 StateIdx = 0; StateIdx < States.Num(); ++StateIdx)
	{
		const FCompactStateTreeState& State = States[StateIdx];
		int64* SourceNodeId = StateIndexToNodeId.Find(StateIdx);
		if (!SourceNodeId) continue;

		for (uint8 TransOffset = 0; TransOffset < State.TransitionsNum; ++TransOffset)
		{
			FStateTreeIndex16 TransIdx(State.TransitionsBegin + TransOffset);
			const FCompactStateTransition* Trans = ST->GetTransitionFromIndex(TransIdx);
			if (!Trans) continue;

			// 只处理有效目标状态的转换
			if (Trans->State.IsValid())
			{
				int32 TargetStateIdx = Trans->State.Index;
				int64* TargetNodeId = StateIndexToNodeId.Find(TargetStateIdx);
				if (TargetNodeId)
				{
					FIndexedConnection Conn;
					Conn.SourceNodeId = *SourceNodeId;
					Conn.TargetNodeId = *TargetNodeId;
					Conn.SourcePin = States[StateIdx].Name.ToString();
					Conn.TargetPin = States[TargetStateIdx].Name.ToString();
					Conn.PinType = TEXT("ST_Transition");
					DB.InsertConnection(Conn);
				}
			}
		}
	}

	// StateTree→TaskClass 交叉引用
	for (const FString& ClassName : UniqueTaskClasses)
	{
		FIndexedConnection Conn;
		Conn.SourceNodeId = AssetId;
		Conn.SourcePin = ST->GetName();
		Conn.TargetPin = ClassName;
		Conn.PinType = TEXT("ST_UsesTaskClass");
		DB.InsertConnection(Conn);
	}
}

#endif // WITH_STATETREE
