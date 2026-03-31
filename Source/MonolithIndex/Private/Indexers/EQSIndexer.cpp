#include "Indexers/EQSIndexer.h"
#include "MonolithSettings.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

static bool SafeCallWithSEH_EQS(void(*Func)(void*), void* Context)
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
struct FEQSIndexCallContext
{
	FEQSIndexer* Self;
	FMonolithIndexDatabase* DB;
	int64 AssetId;
	FSoftObjectPath ObjectPath;
	bool bSuccess;
};

static void LoadAndIndexEQSAsset(void* Ctx)
{
	auto* C = static_cast<FEQSIndexCallContext*>(Ctx);
	C->bSuccess = false;

	UObject* Loaded = C->ObjectPath.TryLoad();
	if (!Loaded) return;

	if (UEnvQuery* Query = Cast<UEnvQuery>(Loaded))
	{
		C->Self->IndexEnvQueryPublic(Query, *C->DB, C->AssetId);
		C->bSuccess = true;
	}
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

FString FEQSIndexer::JsonToString(TSharedPtr<FJsonObject> JsonObj)
{
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(JsonObj, *Writer, true);
	return Out;
}

// ─────────────────────────────────────────────────────────────
// Sentinel 入口
// ─────────────────────────────────────────────────────────────

bool FEQSIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UEnvQuery::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, Assets);

	int32 TotalIndexed = 0;
	for (const FAssetData& AD : Assets)
	{
		int64 AId = DB.GetAssetId(AD.PackageName.ToString());
		if (AId < 0) continue;

		FEQSIndexCallContext Ctx;
		Ctx.Self = this;
		Ctx.DB = &DB;
		Ctx.AssetId = AId;
		Ctx.ObjectPath = AD.GetSoftObjectPath();
		Ctx.bSuccess = false;

#if PLATFORM_WINDOWS
		if (SafeCallWithSEH_EQS(&LoadAndIndexEQSAsset, &Ctx))
		{
			if (Ctx.bSuccess) TotalIndexed++;
		}
		else
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("EQSIndexer: crashed loading/indexing '%s' - skipping"), *AD.GetSoftObjectPath().ToString());
		}
#else
		LoadAndIndexEQSAsset(&Ctx);
		if (Ctx.bSuccess) TotalIndexed++;
#endif
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("EQSIndexer: indexed %d EQS assets"), TotalIndexed);
	return true;
}

// ─────────────────────────────────────────────────────────────
// EnvQuery 索引
// ─────────────────────────────────────────────────────────────

void FEQSIndexer::IndexEnvQuery(UEnvQuery* Query, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Query) return;

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();

	// 收集唯一 Generator/Test 类用于交叉引用
	TSet<FString> UniqueClasses;

	for (int32 i = 0; i < Options.Num(); ++i)
	{
		const UEnvQueryOption* Option = Options[i];
		if (!Option) continue;

		auto Props = MakeShared<FJsonObject>();

		// Generator 信息
		FString GeneratorClassName = TEXT("None");
		if (Option->Generator)
		{
			GeneratorClassName = Option->Generator->GetClass()->GetName();
			Props->SetStringField(TEXT("generator_class"), GeneratorClassName);
			UniqueClasses.Add(GeneratorClassName);
		}

		// Tests 列表
		TArray<TSharedPtr<FJsonValue>> TestsArr;
		for (const UEnvQueryTest* Test : Option->Tests)
		{
			if (!Test) continue;
			const FString TestClassName = Test->GetClass()->GetName();
			TestsArr.Add(MakeShared<FJsonValueString>(TestClassName));
			UniqueClasses.Add(TestClassName);
		}
		Props->SetArrayField(TEXT("tests"), TestsArr);

		FIndexedNode Node;
		Node.AssetId = AssetId;
		Node.NodeType = TEXT("EQS_Option");
		Node.NodeName = FString::Printf(TEXT("Option_%d"), i);
		Node.NodeClass = GeneratorClassName;
		Node.Properties = JsonToString(Props);
		DB.InsertNode(Node);
	}

	// EQS→Generator/Test 类交叉引用
	for (const FString& ClassName : UniqueClasses)
	{
		FIndexedConnection Conn;
		Conn.SourceNodeId = AssetId;
		Conn.SourcePin = Query->GetName();
		Conn.TargetPin = ClassName;
		Conn.PinType = TEXT("EQS_UsesClass");
		DB.InsertConnection(Conn);
	}
}
