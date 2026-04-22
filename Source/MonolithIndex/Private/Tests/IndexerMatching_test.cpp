#include "AssetRegistry/AssetData.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Indexers/ConfigIndexer.h"
#include "Indexers/CppIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/GameplayTagDefinitionIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithIndexDatabase.h"
#include "SQLitePreparedStatement.h"

/*
 * 这组测试专门守住“indexer 到底该不该命中某份资产”的协议。
 *
 * 这件事看起来像小细节，其实很关键：
 * - companion 如果命中过宽，会把所有资产都白跑一遍；
 * - companion 如果命中过窄，又会悄悄漏掉应该补写的数据；
 * - GAS 这种 Blueprint companion 还要额外看父类标签，不能只看短类名。
 */

namespace IndexerMatchingTestInternal
{
	/** 构造一份最小可用的 AssetData，方便单元测试直接喂给 indexer。 */
	static FAssetData MakeAssetData(
		const FString& PackageName,
		const FString& PackagePath,
		const FString& AssetName,
		const FTopLevelAssetPath& AssetClassPath,
		FAssetDataTagMap Tags = FAssetDataTagMap())
	{
		return FAssetData(
			FName(*PackageName),
			FName(*PackagePath),
			FName(*AssetName),
			AssetClassPath,
			MoveTemp(Tags));
	}

	/** 构造一份 Blueprint AssetData，并可选写入 ParentClass / NativeParentClass。 */
	static FAssetData MakeBlueprintAssetData(
		const FString& AssetName,
		const FString& ParentClassValue,
		const bool bUseNativeParentTag)
	{
		FAssetDataTagMap Tags;
		if (!ParentClassValue.IsEmpty())
		{
			Tags.Add(
				bUseNativeParentTag ? FName(TEXT("NativeParentClass")) : FName(TEXT("ParentClass")),
				ParentClassValue);
		}

		return MakeAssetData(
			FString::Printf(TEXT("/Game/Test/%s"), *AssetName),
			TEXT("/Game/Test"),
			AssetName,
			UBlueprint::StaticClass()->GetClassPathName(),
			MoveTemp(Tags));
	}

	/** 每条需要 SQLite 的测试都拿一份临时数据库，避免互相污染。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithIndexerMatch"), TEXT(".db"));
	}

	/** 统计配置表里是否出现了我们刚写进去的那一条唯一配置。 */
	static int64 CountConfigRows(
		FMonolithIndexDatabase& DB,
		const FString& Section,
		const FString& Key,
		const FString& Value)
	{
		FSQLiteDatabase* RawDatabase = DB.GetRawDatabase();
		if (!RawDatabase)
		{
			return -1;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(
			*RawDatabase,
			TEXT("SELECT COUNT(*) FROM configs WHERE section = ? AND key = ? AND value = ?;")))
		{
			return -1;
		}

		Stmt.SetBindingValueByIndex(1, Section);
		Stmt.SetBindingValueByIndex(2, Key);
		Stmt.SetBindingValueByIndex(3, Value);

		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return -1;
		}

		int64 Count = 0;
		Stmt.GetColumnValueByIndex(0, Count);
		return Count;
	}

	/** 统计 C++ 符号表里某个唯一名字的符号有没有被抓到。 */
	static int64 CountCppSymbols(FMonolithIndexDatabase& DB, const FString& SymbolName, const FString& SymbolType)
	{
		FSQLiteDatabase* RawDatabase = DB.GetRawDatabase();
		if (!RawDatabase)
		{
			return -1;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(
			*RawDatabase,
			TEXT("SELECT COUNT(*) FROM cpp_symbols WHERE symbol_name = ? AND symbol_type = ?;")))
		{
			return -1;
		}

		Stmt.SetBindingValueByIndex(1, SymbolName);
		Stmt.SetBindingValueByIndex(2, SymbolType);

		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return -1;
		}

		int64 Count = 0;
		Stmt.GetColumnValueByIndex(0, Count);
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchDefaultClassTest,
	"Monolith.Index.IndexerMatch.DefaultClassDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchDefaultClassTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FMeshCatalogIndexer MeshCatalogIndexer;
	const FAssetData StaticMeshAsset = IndexerMatchingTestInternal::MakeAssetData(
		TEXT("/Game/Test/SM_Crate"),
		TEXT("/Game/Test"),
		TEXT("SM_Crate"),
		UStaticMesh::StaticClass()->GetClassPathName());
	const FAssetData BlueprintAsset = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("BP_Thing"),
		TEXT("/Script/Engine.Actor"),
		true);

	TestTrue(TEXT("mesh catalog should match static mesh assets"), MeshCatalogIndexer.MatchesAsset(StaticMeshAsset));
	TestFalse(TEXT("mesh catalog should not match blueprint assets"), MeshCatalogIndexer.MatchesAsset(BlueprintAsset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchGlobalCompanionTest,
	"Monolith.Index.IndexerMatch.GlobalCompanions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchGlobalCompanionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FDependencyIndexer DependencyIndexer;
	FGameplayTagIndexer GameplayTagIndexer;
	const FAssetData ArbitraryAsset = IndexerMatchingTestInternal::MakeAssetData(
		TEXT("/Game/Test/AnyAsset"),
		TEXT("/Game/Test"),
		TEXT("AnyAsset"),
		UStaticMesh::StaticClass()->GetClassPathName());

	TestTrue(TEXT("dependency companion should match any real package asset"), DependencyIndexer.MatchesAsset(ArbitraryAsset));
	TestTrue(TEXT("gameplay tag companion should match any real package asset"), GameplayTagIndexer.MatchesAsset(ArbitraryAsset));
	TestFalse(TEXT("dependency companion should reject an empty asset record"), DependencyIndexer.MatchesAsset(FAssetData()));
	TestFalse(TEXT("gameplay tag companion should reject an empty asset record"), GameplayTagIndexer.MatchesAsset(FAssetData()));
	TestEqual(TEXT("dependency companion should no longer rely on fake supported classes"), DependencyIndexer.GetSupportedClasses().Num(), 0);
	TestEqual(TEXT("gameplay tag companion should no longer rely on fake supported classes"), GameplayTagIndexer.GetSupportedClasses().Num(), 0);
	TestFalse(TEXT("dependency companion should no longer advertise sentinel mode"), DependencyIndexer.IsSentinel());
	TestFalse(TEXT("gameplay tag companion should no longer advertise sentinel mode"), GameplayTagIndexer.IsSentinel());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchGlobalReducerExecutionTest,
	"Monolith.Index.IndexerMatch.GlobalReducerExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchGlobalReducerExecutionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FConfigIndexer ConfigIndexer;
	FCppIndexer CppIndexer;
	FGameplayTagDefinitionIndexer GameplayTagDefinitionIndexer;

	TestEqual(TEXT("config indexer should expose a stable global reducer id"), ConfigIndexer.GetIndexerId(), FName(TEXT("Config")));
	TestEqual(TEXT("cpp indexer should expose a stable global reducer id"), CppIndexer.GetIndexerId(), FName(TEXT("Cpp")));
	TestEqual(TEXT("gameplay tag definition indexer should expose a stable global reducer id"), GameplayTagDefinitionIndexer.GetIndexerId(), FName(TEXT("GameplayTagDefinitions")));
	TestEqual(TEXT("config indexer should opt into global reducer execution mode"), ConfigIndexer.GetExecutionMode(), EMonolithExecutionMode::GlobalReducer);
	TestEqual(TEXT("cpp indexer should opt into global reducer execution mode"), CppIndexer.GetExecutionMode(), EMonolithExecutionMode::GlobalReducer);
	TestEqual(TEXT("gameplay tag definition indexer should opt into global reducer execution mode"), GameplayTagDefinitionIndexer.GetExecutionMode(), EMonolithExecutionMode::GlobalReducer);
	TestEqual(TEXT("config indexer should no longer rely on fake supported classes"), ConfigIndexer.GetSupportedClasses().Num(), 0);
	TestEqual(TEXT("cpp indexer should no longer rely on fake supported classes"), CppIndexer.GetSupportedClasses().Num(), 0);
	TestEqual(TEXT("gameplay tag definition indexer should no longer rely on fake supported classes"), GameplayTagDefinitionIndexer.GetSupportedClasses().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchConfigGlobalIndexTest,
	"Monolith.Index.IndexerMatch.ConfigGlobalIndexWritesRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchConfigGlobalIndexTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString SectionName = TEXT("/Script/EngineSettings.GeneralProjectSettings");
	const FString KeyName = TEXT("ProjectID");
	const FString ValueName = TEXT("ED25004C416E31430A827AB43F54A4BF");

	const FString DatabasePath = IndexerMatchingTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DatabasePath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("temporary database should open"), DB.Open(DatabasePath));

	FConfigIndexer ConfigIndexer;
	TestTrue(TEXT("config global index should succeed"), ConfigIndexer.IndexGlobal(DB));

	TestEqual(
		TEXT("known project config row should be materialized into configs table exactly once"),
		IndexerMatchingTestInternal::CountConfigRows(DB, SectionName, KeyName, ValueName),
		static_cast<int64>(1));

	DB.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchCppGlobalIndexTest,
	"Monolith.Index.IndexerMatch.CppGlobalIndexWritesRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchCppGlobalIndexTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString UniqueSuffix = LexToString(FPlatformTime::Cycles64());
	const FString TestDirectory = FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Intermediate") / FString::Printf(TEXT("MonolithCppIndexerTest_%s"), *UniqueSuffix);
	const FString HeaderPath = TestDirectory / FString::Printf(TEXT("MonolithCppIndexerTest_%s.h"), *UniqueSuffix);
	const FString ClassName = FString::Printf(TEXT("UMonolithCppIndexerTest_%s"), *UniqueSuffix);
	const FString FunctionName = FString::Printf(TEXT("MonolithCppIndexerTestFunction_%s"), *UniqueSuffix);
	const FString PropertyName = FString::Printf(TEXT("MonolithCppIndexerTestProperty_%s"), *UniqueSuffix);
	const FString HeaderContent = FString::Printf(
		TEXT("UCLASS()\nclass %s : public UObject\n{\n\tGENERATED_BODY()\npublic:\n\tUFUNCTION()\n\tvoid %s();\n\n\tUPROPERTY()\n\tint32 %s;\n};\n"),
		*ClassName,
		*FunctionName,
		*PropertyName);

	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*HeaderPath, false, true);
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	TestTrue(TEXT("temporary cpp test directory should be created"), IFileManager::Get().MakeDirectory(*TestDirectory, true));
	TestTrue(TEXT("temporary header file should be written"), FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath));

	const FString DatabasePath = IndexerMatchingTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DatabasePath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("temporary database should open"), DB.Open(DatabasePath));

	FCppIndexer CppIndexer;
	TestTrue(TEXT("cpp global index should succeed"), CppIndexer.IndexGlobal(DB));
	TestEqual(
		TEXT("temporary class symbol should be discovered exactly once"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, ClassName, TEXT("Class")),
		static_cast<int64>(1));
	TestEqual(
		TEXT("temporary function symbol should be discovered exactly once"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, FunctionName, TEXT("Function")),
		static_cast<int64>(1));
	TestEqual(
		TEXT("temporary property symbol should be discovered exactly once"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, PropertyName, TEXT("Property")),
		static_cast<int64>(1));

	DB.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchConfigGlobalArtifactTest,
	"Monolith.Index.IndexerMatch.ConfigGlobalArtifactRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchConfigGlobalArtifactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString SectionName = TEXT("/Script/EngineSettings.GeneralProjectSettings");
	const FString KeyName = TEXT("ProjectID");
	const FString ValueName = TEXT("ED25004C416E31430A827AB43F54A4BF");
	const FString DatabasePath = IndexerMatchingTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DatabasePath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("temporary database should open"), DB.Open(DatabasePath));

	FConfigIndexer ConfigIndexer;
	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("config artifact identity should build"), ConfigIndexer.BuildGlobalArtifactIdentity(Identity));
	TestEqual(TEXT("config artifact identity should use manifest provider"), Identity.IdentityProvider, EMonolithIdentityProvider::ManifestV1);

	FMonolithArtifact Artifact;
	TestTrue(TEXT("config artifact should build"), ConfigIndexer.BuildGlobalArtifact(Artifact));
	TestTrue(TEXT("config artifact should materialize"), ConfigIndexer.MaterializeGlobalArtifact(Artifact, DB));
	TestTrue(TEXT("config artifact should be idempotent when materialized twice"), ConfigIndexer.MaterializeGlobalArtifact(Artifact, DB));
	TestEqual(
		TEXT("config artifact materialization should keep a unique row snapshot"),
		IndexerMatchingTestInternal::CountConfigRows(DB, SectionName, KeyName, ValueName),
		static_cast<int64>(1));

	DB.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchCppGlobalArtifactTest,
	"Monolith.Index.IndexerMatch.CppGlobalArtifactRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchCppGlobalArtifactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString UniqueSuffix = LexToString(FPlatformTime::Cycles64());
	const FString TestDirectory = FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Intermediate") / FString::Printf(TEXT("MonolithCppArtifactTest_%s"), *UniqueSuffix);
	const FString HeaderPath = TestDirectory / FString::Printf(TEXT("MonolithCppArtifactTest_%s.h"), *UniqueSuffix);
	const FString ClassName = FString::Printf(TEXT("UMonolithCppArtifactTest_%s"), *UniqueSuffix);
	const FString FunctionName = FString::Printf(TEXT("MonolithCppArtifactFunction_%s"), *UniqueSuffix);
	const FString PropertyName = FString::Printf(TEXT("MonolithCppArtifactProperty_%s"), *UniqueSuffix);
	const FString HeaderContent = FString::Printf(
		TEXT("UCLASS()\nclass %s : public UObject\n{\n\tGENERATED_BODY()\npublic:\n\tUFUNCTION()\n\tvoid %s();\n\n\tUPROPERTY()\n\tint32 %s;\n};\n"),
		*ClassName,
		*FunctionName,
		*PropertyName);

	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*HeaderPath, false, true);
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	TestTrue(TEXT("temporary cpp artifact test directory should be created"), IFileManager::Get().MakeDirectory(*TestDirectory, true));
	TestTrue(TEXT("temporary cpp artifact header should be written"), FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath));

	const FString DatabasePath = IndexerMatchingTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DatabasePath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("temporary database should open"), DB.Open(DatabasePath));

	FCppIndexer CppIndexer;
	FMonolithArtifactIdentityV1 Identity;
	TestTrue(TEXT("cpp artifact identity should build"), CppIndexer.BuildGlobalArtifactIdentity(Identity));
	TestEqual(TEXT("cpp artifact identity should use manifest provider"), Identity.IdentityProvider, EMonolithIdentityProvider::ManifestV1);

	FMonolithArtifact Artifact;
	TestTrue(TEXT("cpp artifact should build"), CppIndexer.BuildGlobalArtifact(Artifact));
	TestTrue(TEXT("cpp artifact should materialize"), CppIndexer.MaterializeGlobalArtifact(Artifact, DB));
	TestTrue(TEXT("cpp artifact should be idempotent when materialized twice"), CppIndexer.MaterializeGlobalArtifact(Artifact, DB));
	TestEqual(
		TEXT("cpp artifact materialization should keep class symbol unique"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, ClassName, TEXT("Class")),
		static_cast<int64>(1));
	TestEqual(
		TEXT("cpp artifact materialization should keep function symbol unique"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, FunctionName, TEXT("Function")),
		static_cast<int64>(1));
	TestEqual(
		TEXT("cpp artifact materialization should keep property symbol unique"),
		IndexerMatchingTestInternal::CountCppSymbols(DB, PropertyName, TEXT("Property")),
		static_cast<int64>(1));

	DB.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerMatchGASTest,
	"Monolith.Index.IndexerMatch.GASBlueprintParents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerMatchGASTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FGASIndexer GASIndexer;

	const FAssetData AbilityBlueprint = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("GA_Test"),
		TEXT("/Script/GameplayAbilities.GameplayAbility"),
		true);
	const FAssetData EffectBlueprint = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("GE_Test"),
		TEXT("/Script/GameplayAbilities.GameplayEffect"),
		true);
	const FAssetData AttributeSetBlueprint = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("AS_Test"),
		TEXT("/Script/GameplayAbilities.AttributeSet"),
		false);
	const FAssetData CueBlueprint = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("GC_Test"),
		TEXT("/Script/GameplayAbilities.GameplayCueNotify_Actor"),
		false);
	const FAssetData NonGASBlueprint = IndexerMatchingTestInternal::MakeBlueprintAssetData(
		TEXT("BP_Actor"),
		TEXT("/Script/Engine.Actor"),
		true);
	const FAssetData StaticMeshAsset = IndexerMatchingTestInternal::MakeAssetData(
		TEXT("/Game/Test/SM_Box"),
		TEXT("/Game/Test"),
		TEXT("SM_Box"),
		UStaticMesh::StaticClass()->GetClassPathName());

	TestTrue(TEXT("GAS indexer should match gameplay ability blueprints"), GASIndexer.MatchesAsset(AbilityBlueprint));
	TestTrue(TEXT("GAS indexer should match gameplay effect blueprints"), GASIndexer.MatchesAsset(EffectBlueprint));
	TestTrue(TEXT("GAS indexer should match attribute set blueprints"), GASIndexer.MatchesAsset(AttributeSetBlueprint));
	TestTrue(TEXT("GAS indexer should match gameplay cue blueprints"), GASIndexer.MatchesAsset(CueBlueprint));
	TestFalse(TEXT("GAS indexer should reject ordinary actor blueprints"), GASIndexer.MatchesAsset(NonGASBlueprint));
	TestFalse(TEXT("GAS indexer should reject non-blueprint assets"), GASIndexer.MatchesAsset(StaticMeshAsset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexerGASIndexAssetTest,
	"Monolith.Index.IndexerMatch.GASIndexAssetWritesNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexerGASIndexAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString DatabasePath = IndexerMatchingTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DatabasePath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("temporary database should open"), DB.Open(DatabasePath));

	const FString UniqueSuffix = LexToString(FPlatformTime::Cycles64());
	const FString PackageName = FString::Printf(TEXT("/MonolithIndexTests/BP_GA_Index_%s"), *UniqueSuffix);
	UPackage* Package = CreatePackage(*PackageName);
	TestNotNull(TEXT("test package should be created"), Package);

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UGameplayAbility::StaticClass(),
		Package,
		FName(TEXT("BP_GA_Index")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("MonolithIndexIndexerMatchTest")));
	TestNotNull(TEXT("gameplay ability blueprint should be created"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	const FAssetData AssetData(Blueprint);
	FGASIndexer GASIndexer;
	TestTrue(TEXT("loaded gameplay ability blueprint should match GAS indexer"), GASIndexer.MatchesAsset(AssetData, Blueprint));

	FIndexedAsset IndexedAsset;
	IndexedAsset.PackagePath = AssetData.PackageName.ToString();
	IndexedAsset.AssetName = AssetData.AssetName.ToString();
	IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
	const int64 AssetId = DB.InsertAsset(IndexedAsset);
	TestTrue(TEXT("test asset row should be inserted"), AssetId > 0);

	TestTrue(TEXT("GAS indexer should write a node for gameplay ability blueprint"), GASIndexer.IndexAsset(AssetData, Blueprint, DB, AssetId));

	const TArray<FIndexedNode> Nodes = DB.GetNodesForAsset(AssetId);
	TestEqual(TEXT("GAS gameplay ability index should emit exactly one node"), Nodes.Num(), 1);
	if (Nodes.Num() == 1)
	{
		TestEqual(TEXT("GAS gameplay ability node type should be stable"), Nodes[0].NodeType, FString(TEXT("GameplayAbility")));
		TestTrue(TEXT("GAS gameplay ability properties should include asset path"), Nodes[0].Properties.Contains(TEXT("\"asset_path\"")));
	}

	DB.Close();
	return true;
}
