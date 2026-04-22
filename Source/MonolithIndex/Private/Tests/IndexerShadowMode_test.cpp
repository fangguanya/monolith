#include "Misc/AutomationTest.h"
#include "Abilities/GameplayAbility.h"
#include "Animation/AnimMontage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/Level.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Indexers/AnimationIndexer.h"
#include "Indexers/BehaviorTreeIndexer.h"
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/EQSIndexer.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/InputActionIndexer.h"
#include "Indexers/LevelIndexer.h"
#include "Indexers/MaterialIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "Indexers/NiagaraIndexer.h"
#if WITH_STATETREE
#include "Indexers/StateTreeIndexer.h"
#endif
#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/UserDefinedStructIndexer.h"
#include "InputAction.h"
#include "EdGraphSchema_K2.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexerShadowMode.h"
#include "NiagaraSystem.h"
#include "SQLitePreparedStatement.h"
#include "StructUtils/UserDefinedStruct.h"

/*
 * 这组测试是 shadow mode 的“总说明书”之一。
 *
 * 它覆盖的范围比较广：
 * - cohort 配置和 diff 判定；
 * - 稳定采样和稳定 row hash；
 * - retention 清理；
 * - 多个 artifact-capable indexer 往 shadow 表写入的基本闭环。
 */

namespace MonolithShadowModeTestInternal
{
	/** 每条测试各自用一份临时数据库，互不干扰。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithShadowMode"), TEXT(".db"));
	}

	/** 检查某张表是否真的存在于 SQLite 里。 */
	static bool DoesTableExist(FMonolithIndexDatabase& DB, const FString& TableName)
	{
		FSQLiteDatabase* RawDatabase = DB.GetRawDatabase();
		if (!RawDatabase)
		{
			return false;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDatabase, TEXT("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;")))
		{
			return false;
		}

		Stmt.SetBindingValueByIndex(1, TableName);
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	}

	/** 测试里手工拼一个 mesh catalog artifact payload。
	 * 生产代码有自己的私有序列化 helper；这里复制最小必要格式，只为了验证 materialize 端。 */
	static void SerializeMeshCatalogArtifact(const FIndexedMeshCatalogEntry& Entry, TArray<uint8>& OutBytes)
	{
		auto WriteUInt8 = [&OutBytes](const uint8 Value)
		{
			OutBytes.Add(Value);
		};
		auto WriteUInt32 = [&OutBytes](const uint32 Value)
		{
			for (int32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
			{
				OutBytes.Add(static_cast<uint8>((Value >> (ByteIndex * 8)) & 0xff));
			}
		};
		auto WriteUInt64 = [&OutBytes](const uint64 Value)
		{
			for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
			{
				OutBytes.Add(static_cast<uint8>((Value >> (ByteIndex * 8)) & 0xff));
			}
		};
		auto WriteDouble = [&WriteUInt64](const double Value)
		{
			uint64 Bits = 0;
			FMemory::Memcpy(&Bits, &Value, sizeof(double));
			WriteUInt64(Bits);
		};
		auto WriteString = [&WriteUInt32, &OutBytes](const FString& Value)
		{
			FTCHARToUTF8 Utf8(*Value);
			WriteUInt32(static_cast<uint32>(Utf8.Length()));
			if (Utf8.Length() > 0)
			{
				OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			}
		};

		OutBytes.Reset();
		WriteUInt8(1);
		WriteString(Entry.AssetPath);
		WriteDouble(Entry.BoundsX);
		WriteDouble(Entry.BoundsY);
		WriteDouble(Entry.BoundsZ);
		WriteDouble(Entry.BoundsMin);
		WriteDouble(Entry.BoundsMid);
		WriteDouble(Entry.BoundsMax);
		WriteDouble(Entry.Volume);
		WriteString(Entry.SizeClass);
		WriteString(Entry.Category);
		WriteUInt32(static_cast<uint32>(Entry.TriCount));
		WriteUInt8(Entry.bHasCollision ? 1 : 0);
		WriteUInt32(static_cast<uint32>(Entry.LodCount));
		WriteDouble(Entry.PivotOffsetZ);
		WriteUInt8(Entry.bDegenerate ? 1 : 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeSingleCohortTest,
	"Monolith.Index.ShadowMode.AllowsOnlyOneCohortAtATime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeSingleCohortTest::RunTest(const FString& Parameters)
{
	// shadow mode 当前只允许单 cohort，避免一次比较太多张表导致诊断混乱。
	FName CohortName;
	TestTrue(TEXT("single cohort should parse"), ParseShadowModeCohort(TEXT("GenericAsset"), CohortName));
	TestEqual(TEXT("parsed cohort should be preserved"), CohortName, FName(TEXT("GenericAsset")));
	TestFalse(TEXT("multiple cohorts should be rejected"), ParseShadowModeCohort(TEXT("Blueprint,Material"), CohortName));
	TestFalse(TEXT("empty cohort should be rejected"), ParseShadowModeCohort(TEXT("   "), CohortName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeLevel1ThenLevel2DiffTest,
	"Monolith.Index.ShadowMode.UsesLevel1ThenLevel2Diff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeLevel1ThenLevel2DiffTest::RunTest(const FString& Parameters)
{
	// 先看一级聚合，再决定是否要做更贵的二级逐行比较。
	const FMonolithShadowDiffDecision Match = EvaluateShadowDiff(
		FMonolithShadowAggregate{ 100, 9001 },
		FMonolithShadowAggregate{ 100, 9001 });
	TestFalse(TEXT("matching aggregates should not require level2"), Match.bRequiresLevel2);
	TestFalse(TEXT("matching aggregates should not roll back"), Match.bShouldRollback);

	const FMonolithShadowDiffDecision SmallMismatch = EvaluateShadowDiff(
		FMonolithShadowAggregate{ 1000, 111 },
		FMonolithShadowAggregate{ 1000, 112 });
	TestTrue(TEXT("hash mismatch should trigger level2"), SmallMismatch.bRequiresLevel2);
	TestFalse(TEXT("1/1000 mismatch should stay below rollback threshold"), SmallMismatch.bShouldRollback);

	const FMonolithShadowDiffDecision LargeMismatch = EvaluateShadowDiff(
		FMonolithShadowAggregate{ 1000, 111 },
		FMonolithShadowAggregate{ 998, 222 });
	TestTrue(TEXT("larger mismatch should still require level2"), LargeMismatch.bRequiresLevel2);
	TestTrue(TEXT("aggregate mismatch above 0.1% should request rollback"), LargeMismatch.bShouldRollback);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeDeterministicSamplingTest,
	"Monolith.Index.ShadowMode.Level2SamplingIsDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeDeterministicSamplingTest::RunTest(const FString& Parameters)
{
	// 同一主键必须始终得到同样的抽样结果，否则 Level2 diff 会忽左忽右。
	const FString StableKey = TEXT("GenericAsset|/Game/Test/SM_Cube|Metadata");
	const bool bFirstSample = IsDeterministicLevel2Sample(StableKey);
	const bool bSecondSample = IsDeterministicLevel2Sample(StableKey);
	TestEqual(TEXT("same key should always produce same sampling result"), bFirstSample, bSecondSample);

	int32 SampledCount = 0;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		if (IsDeterministicLevel2Sample(FString::Printf(TEXT("pk-%d"), Index)))
		{
			++SampledCount;
		}
	}

	TestTrue(TEXT("deterministic sampler should hit a small non-zero subset"), SampledCount > 0 && SampledCount < 40);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeLevel2DiffDetectsSampledMismatchTest,
	"Monolith.Index.ShadowMode.Level2DiffDetectsSampledMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeLevel2DiffDetectsSampledMismatchTest::RunTest(const FString& Parameters)
{
	// 这条测试守住真正的 Level 2 语义：
	// - 只比较确定性采样命中的 key；
	// - 同 key row_hash 不同会记成 mismatch；
	// - 未采样 key 的差异不会污染结果。
	FString SampledKey;
	FString UnsampledKey;
	FString SampledShadowOnlyKey;

	for (int32 Index = 0; Index < 5000; ++Index)
	{
		const FString CandidateKey = FString::Printf(TEXT("level2-key-%d"), Index);
		if (SampledKey.IsEmpty() && IsDeterministicLevel2Sample(CandidateKey))
		{
			SampledKey = CandidateKey;
		}
		else if (UnsampledKey.IsEmpty() && !IsDeterministicLevel2Sample(CandidateKey))
		{
			UnsampledKey = CandidateKey;
		}
		else if (SampledShadowOnlyKey.IsEmpty() && CandidateKey != SampledKey && IsDeterministicLevel2Sample(CandidateKey))
		{
			SampledShadowOnlyKey = CandidateKey;
		}

		if (!SampledKey.IsEmpty() && !UnsampledKey.IsEmpty() && !SampledShadowOnlyKey.IsEmpty())
		{
			break;
		}
	}

	TestFalse(TEXT("sampled key should be found"), SampledKey.IsEmpty());
	TestFalse(TEXT("unsampled key should be found"), UnsampledKey.IsEmpty());
	TestFalse(TEXT("sampled shadow-only key should be found"), SampledShadowOnlyKey.IsEmpty());
	if (SampledKey.IsEmpty() || UnsampledKey.IsEmpty() || SampledShadowOnlyKey.IsEmpty())
	{
		return false;
	}

	TArray<FMonolithShadowLevel2Row> ProductionRows;
	ProductionRows.Add(FMonolithShadowLevel2Row{ SampledKey, 11, TEXT("prod-sampled") });
	ProductionRows.Add(FMonolithShadowLevel2Row{ UnsampledKey, 22, TEXT("prod-unsampled") });

	TArray<FMonolithShadowLevel2Row> ShadowRows;
	ShadowRows.Add(FMonolithShadowLevel2Row{ SampledKey, 99, TEXT("shadow-sampled-mismatch") });
	ShadowRows.Add(FMonolithShadowLevel2Row{ UnsampledKey, 33, TEXT("shadow-unsampled-mismatch") });
	ShadowRows.Add(FMonolithShadowLevel2Row{ SampledShadowOnlyKey, 44, TEXT("shadow-only") });

	const FMonolithShadowLevel2DiffResult Result = EvaluateShadowLevel2Diff(ProductionRows, ShadowRows);
	TestEqual(TEXT("sampled production rows should count only sampled keys"), Result.SampledProductionRows, 1u);
	TestEqual(TEXT("sampled shadow rows should include both sampled keys"), Result.SampledShadowRows, 2u);
	TestEqual(TEXT("only one sampled key exists on both sides"), Result.ComparedRows, 1u);
	TestEqual(TEXT("sampled mismatch should be detected"), Result.MismatchedRows, 1u);
	TestEqual(TEXT("sampled shadow-only row should be counted"), Result.ShadowOnlyRows, 1u);
	TestEqual(TEXT("no sampled production-only rows should remain"), Result.ProductionOnlyRows, 0u);
	TestTrue(TEXT("level2 result should report mismatch"), Result.HasMismatch());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeStableRowHashTest,
	"Monolith.Index.ShadowMode.RowHashesIgnoreAssetIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeStableRowHashTest::RunTest(const FString& Parameters)
{
	// row hash 的关键要求是：业务内容相同，asset/revision 变了也不能跟着变。
	FIndexedNode FirstNode;
	FirstNode.AssetId = 10;
	FirstNode.RevisionId = 1;
	FirstNode.NodeType = TEXT("Metadata");
	FirstNode.NodeName = TEXT("SM_Test");
	FirstNode.NodeClass = TEXT("StaticMesh");
	FirstNode.Properties = TEXT("{\"triangles\":12}");
	FirstNode.PosX = 5;
	FirstNode.PosY = 7;

	FIndexedNode SecondNode = FirstNode;
	SecondNode.AssetId = 999;
	SecondNode.RevisionId = 42;

	TestEqual(TEXT("node hash should ignore asset/revision identity"), ComputeNodeRowHash(FirstNode), ComputeNodeRowHash(SecondNode));

	FIndexedParameter FirstParameter;
	FirstParameter.AssetId = 10;
	FirstParameter.RevisionId = 1;
	FirstParameter.ParamName = TEXT("Roughness");
	FirstParameter.ParamType = TEXT("Scalar");
	FirstParameter.ParamGroup = TEXT("Surface");
	FirstParameter.DefaultValue = TEXT("0.5");
	FirstParameter.Source = TEXT("Material");

	FIndexedParameter SecondParameter = FirstParameter;
	SecondParameter.AssetId = 1001;
	SecondParameter.RevisionId = 88;

	TestEqual(TEXT("parameter hash should ignore asset/revision identity"), ComputeParameterRowHash(FirstParameter), ComputeParameterRowHash(SecondParameter));

	FIndexedVariable FirstVariable;
	FirstVariable.AssetId = 50;
	FirstVariable.RevisionId = 7;
	FirstVariable.VarName = TEXT("Health");
	FirstVariable.VarType = TEXT("int");
	FirstVariable.Category = TEXT("Gameplay");
	FirstVariable.DefaultValue = TEXT("100");
	FirstVariable.bIsExposed = true;
	FirstVariable.bIsReplicated = false;

	FIndexedVariable SecondVariable = FirstVariable;
	SecondVariable.AssetId = 9999;
	SecondVariable.RevisionId = 123;

	TestEqual(TEXT("variable hash should ignore asset/revision identity"), ComputeVariableRowHash(FirstVariable), ComputeVariableRowHash(SecondVariable));

	FIndexedActor FirstActor;
	FirstActor.AssetId = 100;
	FirstActor.RevisionId = 1;
	FirstActor.ActorName = TEXT("SM_Decoration");
	FirstActor.ActorClass = TEXT("StaticMeshActor");
	FirstActor.ActorLabel = TEXT("Decoration");
	FirstActor.Transform = TEXT("{\"location\":{\"x\":1}}");
	FirstActor.Components = TEXT("[{\"name\":\"Mesh\"}]");

	FIndexedActor SecondActor = FirstActor;
	SecondActor.AssetId = 200;
	SecondActor.RevisionId = 99;

	TestEqual(TEXT("actor hash should ignore asset/revision identity"), ComputeActorRowHash(FirstActor), ComputeActorRowHash(SecondActor));

	FIndexedDataTableRow FirstRow;
	FirstRow.AssetId = 123;
	FirstRow.RevisionId = 4;
	FirstRow.RowName = TEXT("ItemA");
	FirstRow.RowData = TEXT("{\"cost\":10}");

	FIndexedDataTableRow SecondRow = FirstRow;
	SecondRow.AssetId = 456;
	SecondRow.RevisionId = 9;

	TestEqual(TEXT("datatable row hash should ignore asset/revision identity"), ComputeDataTableRowHash(FirstRow), ComputeDataTableRowHash(SecondRow));

	FIndexedMeshCatalogEntry FirstMeshCatalogEntry;
	FirstMeshCatalogEntry.AssetId = 321;
	FirstMeshCatalogEntry.RevisionId = 2;
	FirstMeshCatalogEntry.AssetPath = TEXT("/Game/Test/SM_RowHash.SM_RowHash");
	FirstMeshCatalogEntry.BoundsX = 100.0;
	FirstMeshCatalogEntry.BoundsY = 50.0;
	FirstMeshCatalogEntry.BoundsZ = 25.0;
	FirstMeshCatalogEntry.BoundsMin = 25.0;
	FirstMeshCatalogEntry.BoundsMid = 50.0;
	FirstMeshCatalogEntry.BoundsMax = 100.0;
	FirstMeshCatalogEntry.Volume = 125000.0;
	FirstMeshCatalogEntry.SizeClass = TEXT("medium");
	FirstMeshCatalogEntry.Category = TEXT("Props.Chair");
	FirstMeshCatalogEntry.TriCount = 128;
	FirstMeshCatalogEntry.bHasCollision = true;
	FirstMeshCatalogEntry.LodCount = 3;
	FirstMeshCatalogEntry.PivotOffsetZ = 12.5;
	FirstMeshCatalogEntry.bDegenerate = false;

	FIndexedMeshCatalogEntry SecondMeshCatalogEntry = FirstMeshCatalogEntry;
	SecondMeshCatalogEntry.AssetId = 654;
	SecondMeshCatalogEntry.RevisionId = 7;

	TestEqual(TEXT("mesh catalog hash should ignore asset/revision identity"), ComputeMeshCatalogRowHash(FirstMeshCatalogEntry), ComputeMeshCatalogRowHash(SecondMeshCatalogEntry));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithShadowModeRetentionDropTest,
	"Monolith.Index.ShadowMode.ShadowTablesDropAfterPromoteRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithShadowModeRetentionDropTest::RunTest(const FString& Parameters)
{
	// promote 后的 shadow 表不会永久保留，过期后应该能被清掉。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_Shadow");
	Asset.AssetName = TEXT("SM_Shadow");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));

	FMonolithShadowIndexedNode ShadowNode;
	ShadowNode.Node.AssetId = AssetId;
	ShadowNode.Node.NodeType = TEXT("Metadata");
	ShadowNode.Node.NodeName = TEXT("SM_Shadow");
	ShadowNode.Node.NodeClass = TEXT("StaticMesh");
	ShadowNode.Node.Properties = TEXT("{\"triangles\":12}");
	ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);

	TArray<FMonolithShadowIndexedNode> ShadowNodes;
	ShadowNodes.Add(ShadowNode);
	TestTrue(TEXT("shadow node should be written"), DB.ReplaceShadowNodesForAsset(TEXT("GenericAsset"), AssetId, ShadowNodes));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FString ShadowTableName = MakeShadowTableName(TEXT("GenericAsset"), TEXT("nodes"));
	TestTrue(TEXT("shadow table should exist before expiry cleanup"), MonolithShadowModeTestInternal::DoesTableExist(DB, ShadowTableName));

	const FMonolithShadowNodeAggregate Aggregate = DB.GetShadowNodeAggregateForAsset(TEXT("GenericAsset"), AssetId);
	TestEqual(TEXT("shadow aggregate should expose written row"), Aggregate.RowCount, 1ull);

	const FDateTime ExpiredAtUtc = FDateTime::UtcNow() - FTimespan::FromMinutes(1.0);
	TestTrue(TEXT("retention should be upserted"), DB.UpsertShadowTableRetention(TEXT("GenericAsset"), TEXT("nodes"), ExpiredAtUtc, false));
	TestEqual(TEXT("expired shadow table should be dropped"), DB.DropExpiredShadowTables(FDateTime::UtcNow()), 1);
	TestFalse(TEXT("shadow table should be gone after expiry cleanup"), MonolithShadowModeTestInternal::DoesTableExist(DB, ShadowTableName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialShadowArtifactParametersTest,
	"Monolith.Index.ShadowMode.MaterialArtifactWritesParameterShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialShadowArtifactParametersTest::RunTest(const FString& Parameters)
{
	// 用一个瞬态 MIC 验证“构建 artifact -> 回放到 shadow parameters”的闭环。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/MI_Shadow");
	Asset.AssetName = TEXT("MI_Shadow");
	Asset.AssetClass = TEXT("MaterialInstanceConstant");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UMaterialInstanceConstant* MaterialInstance = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient material instance should be created"), MaterialInstance);

	FScalarParameterValue ScalarParameter;
	ScalarParameter.ParameterInfo.Name = TEXT("Roughness");
	ScalarParameter.ParameterValue = 0.4f;
	MaterialInstance->ScalarParameterValues.Add(ScalarParameter);

	FVectorParameterValue VectorParameter;
	VectorParameter.ParameterInfo.Name = TEXT("Tint");
	VectorParameter.ParameterValue = FLinearColor(0.2f, 0.4f, 0.6f, 1.0f);
	MaterialInstance->VectorParameterValues.Add(VectorParameter);

	FMaterialIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("material artifact should build from transient MIC"), Indexer.BuildArtifact(FAssetData(), MaterialInstance, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("Material")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowParameterAggregate ParameterAggregate = DB.GetShadowParameterAggregateForAsset(TEXT("Material"), AssetId);
	TestEqual(TEXT("shadow parameter aggregate should contain two rows"), ParameterAggregate.RowCount, 2ull);
	TestTrue(TEXT("shadow parameter hash sum should be non-zero"), ParameterAggregate.RowHashSum != 0ull);

	const FMonolithShadowConnectionAggregate ConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(TEXT("Material"), AssetId);
	TestEqual(TEXT("MIC artifact should not emit connection rows"), ConnectionAggregate.RowCount, 0ull);

	TestTrue(TEXT("shadow parameter retention should be stored"), DB.UpsertShadowTableRetention(TEXT("Material"), TEXT("parameters"), FDateTime::UtcNow() + FTimespan::FromHours(1.0), false));
	const FString ParameterShadowTableName = MakeShadowTableName(TEXT("Material"), TEXT("parameters"));
	TestTrue(TEXT("parameter shadow table should exist"), MonolithShadowModeTestInternal::DoesTableExist(DB, ParameterShadowTableName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintShadowArtifactVariablesTest,
	"Monolith.Index.ShadowMode.BlueprintArtifactWritesVariableShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintShadowArtifactVariablesTest::RunTest(const FString& Parameters)
{
	// 用不带图的瞬态 Blueprint 也能验证变量 shadow 行写入是否正常。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_Shadow");
	Asset.AssetName = TEXT("BP_Shadow");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient blueprint should be created"), Blueprint);

	FBPVariableDescription HealthVariable;
	HealthVariable.VarName = TEXT("Health");
	HealthVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	HealthVariable.Category = FText::FromString(TEXT("Gameplay"));
	HealthVariable.DefaultValue = TEXT("100");
	HealthVariable.PropertyFlags = CPF_ExposeOnSpawn;
	Blueprint->NewVariables.Add(HealthVariable);

	FBPVariableDescription NameVariable;
	NameVariable.VarName = TEXT("DisplayName");
	NameVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_String;
	NameVariable.Category = FText::FromString(TEXT("UI"));
	NameVariable.DefaultValue = TEXT("Hero");
	NameVariable.PropertyFlags = CPF_Net;
	Blueprint->NewVariables.Add(NameVariable);

	FBlueprintIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("blueprint artifact should build from transient blueprint"), Indexer.BuildArtifact(FAssetData(), Blueprint, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow blueprint materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("Blueprint")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowVariableAggregate VariableAggregate = DB.GetShadowVariableAggregateForAsset(TEXT("Blueprint"), AssetId);
	TestEqual(TEXT("shadow variable aggregate should contain two rows"), VariableAggregate.RowCount, 2ull);
	TestTrue(TEXT("shadow variable hash sum should be non-zero"), VariableAggregate.RowHashSum != 0ull);

	const FMonolithShadowConnectionAggregate ConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(TEXT("Blueprint"), AssetId);
	TestEqual(TEXT("graphless transient blueprint should not emit connection rows"), ConnectionAggregate.RowCount, 0ull);

	TestTrue(TEXT("shadow variable retention should be stored"), DB.UpsertShadowTableRetention(TEXT("Blueprint"), TEXT("variables"), FDateTime::UtcNow() + FTimespan::FromHours(1.0), false));
	const FString VariableShadowTableName = MakeShadowTableName(TEXT("Blueprint"), TEXT("variables"));
	TestTrue(TEXT("variable shadow table should exist"), MonolithShadowModeTestInternal::DoesTableExist(DB, VariableShadowTableName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelShadowArtifactActorsTest,
	"Monolith.Index.ShadowMode.LevelArtifactWritesActorShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelShadowArtifactActorsTest::RunTest(const FString& Parameters)
{
	// Level 的影子链路重点看 actors 聚合，而不是旧的 sentinel world 节点。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/L_Shadow");
	Asset.AssetName = TEXT("L_Shadow");
	Asset.AssetClass = TEXT("World");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient world should be created"), World);
	World->PersistentLevel = NewObject<ULevel>(World, NAME_None, RF_Transient);
	TestNotNull(TEXT("persistent level should be created"), World->PersistentLevel.Get());

	AActor* Actor = NewObject<AActor>(World->PersistentLevel, TEXT("ShadowActor"), RF_Transient);
	TestNotNull(TEXT("transient actor should be created"), Actor);
	World->PersistentLevel->Actors.Add(Actor);

	FLevelIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("level artifact should build from transient world"), Indexer.BuildArtifact(FAssetData(), World, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow level materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("Level")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowActorAggregate ActorAggregate = DB.GetShadowActorAggregateForAsset(TEXT("Level"), AssetId);
	TestEqual(TEXT("shadow actor aggregate should contain one row"), ActorAggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow actor hash sum should be non-zero"), ActorAggregate.RowHashSum != 0ull);

	TestTrue(TEXT("shadow actor retention should be stored"), DB.UpsertShadowTableRetention(TEXT("Level"), TEXT("actors"), FDateTime::UtcNow() + FTimespan::FromHours(1.0), false));
	const FString ActorShadowTableName = MakeShadowTableName(TEXT("Level"), TEXT("actors"));
	TestTrue(TEXT("actor shadow table should exist"), MonolithShadowModeTestInternal::DoesTableExist(DB, ActorShadowTableName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInputActionShadowArtifactNodesTest,
	"Monolith.Index.ShadowMode.InputActionArtifactWritesNodeShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInputActionShadowArtifactNodesTest::RunTest(const FString& Parameters)
{
	// InputAction 属于轻量 package-scoped node cohort。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/IA_Shadow");
	Asset.AssetName = TEXT("IA_Shadow");
	Asset.AssetClass = TEXT("InputAction");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UInputAction* InputAction = NewObject<UInputAction>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient input action should be created"), InputAction);
	InputAction->ValueType = EInputActionValueType::Axis2D;
	InputAction->ActionDescription = FText::FromString(TEXT("Shadow Jump"));
	InputAction->bConsumeInput = true;
	InputAction->bTriggerWhenPaused = false;

	FInputActionIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("input action artifact should build from transient action"), Indexer.BuildArtifact(FAssetData(), InputAction, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow input action materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("InputAction")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("InputAction"), AssetId);
	TestEqual(TEXT("shadow node aggregate should contain one row"), NodeAggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow node hash sum should be non-zero"), NodeAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASShadowArtifactNodesTest,
	"Monolith.Index.ShadowMode.GASArtifactWritesNodeShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASShadowArtifactNodesTest::RunTest(const FString& Parameters)
{
	// GAS companion 现在也必须能走 artifact -> shadow 这条统一主链，
	// 不能再只依赖直接写生产表的旧实现。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	const FString UniqueSuffix = LexToString(FPlatformTime::Cycles64());
	const FString PackageName = FString::Printf(TEXT("/MonolithIndexTests/BP_GA_Shadow_%s"), *UniqueSuffix);
	UPackage* Package = CreatePackage(*PackageName);
	TestNotNull(TEXT("test package should be created"), Package);

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UGameplayAbility::StaticClass(),
		Package,
		FName(TEXT("BP_GA_Shadow")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("MonolithIndexShadowModeTest")));
	TestNotNull(TEXT("gameplay ability blueprint should be created"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	const FAssetData AssetData(Blueprint);
	FIndexedAsset Asset;
	Asset.PackagePath = AssetData.PackageName.ToString();
	Asset.AssetName = AssetData.AssetName.ToString();
	Asset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	FGASIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("GAS artifact should build from gameplay ability blueprint"), Indexer.BuildArtifact(AssetData, Blueprint, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow GAS materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("GAS")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("GAS"), AssetId);
	TestEqual(TEXT("shadow GAS node aggregate should contain one row"), NodeAggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow GAS node hash sum should be non-zero"), NodeAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAnimationShadowArtifactNodesTest,
	"Monolith.Index.ShadowMode.AnimationArtifactWritesNodeShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationShadowArtifactNodesTest::RunTest(const FString& Parameters)
{
	// Animation 这里先验证最轻量的 node shadow 落地闭环。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/AM_Shadow");
	Asset.AssetName = TEXT("AM_Shadow");
	Asset.AssetClass = TEXT("AnimMontage");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UAnimMontage* Montage = NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient animation montage should be created"), Montage);

	FAnimationIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("animation artifact should build from transient montage"), Indexer.BuildArtifact(FAssetData(), Montage, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow animation materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("Animation")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("Animation"), AssetId);
	TestEqual(TEXT("shadow node aggregate should contain one row"), NodeAggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow node hash sum should be non-zero"), NodeAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraShadowArtifactNodesTest,
	"Monolith.Index.ShadowMode.NiagaraArtifactWritesNodeShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraShadowArtifactNodesTest::RunTest(const FString& Parameters)
{
	// Niagara 现在不再走全局 sentinel，而是像普通资产一样生成一组 node 快照。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/NS_Shadow");
	Asset.AssetName = TEXT("NS_Shadow");
	Asset.AssetClass = TEXT("NiagaraSystem");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient niagara system should be created"), NiagaraSystem);
	NiagaraSystem->bFixedBounds = true;
	NiagaraSystem->SetFixedBounds(FBox(FVector(-10.0, -5.0, -2.0), FVector(10.0, 5.0, 2.0)));

	FNiagaraIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("niagara artifact should build from transient system"), Indexer.BuildArtifact(FAssetData(), NiagaraSystem, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow niagara materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("Niagara")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("Niagara"), AssetId);
	TestEqual(TEXT("shadow niagara aggregate should contain one system row"), NodeAggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow niagara hash sum should be non-zero"), NodeAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBehaviorTreeShadowArtifactGraphTest,
	"Monolith.Index.ShadowMode.BehaviorTreeArtifactWritesVariableAndConnectionShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBehaviorTreeShadowArtifactGraphTest::RunTest(const FString& Parameters)
{
	// BehaviorTree cohort 现在统一走 graph payload：
	// 一个资产可以携带 nodes、variables、connections 中的任意组合。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BT_Shadow");
	Asset.AssetName = TEXT("BT_Shadow");
	Asset.AssetClass = TEXT("BehaviorTree");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	FIndexedNode RootNode;
	RootNode.NodeType = TEXT("BT_Composite");
	RootNode.NodeName = TEXT("Selector");
	RootNode.NodeClass = TEXT("BTComposite_Selector");
	RootNode.Properties = TEXT("{\"execution_index\":0}");
	Payload.Nodes.Add(RootNode);

	FIndexedNode TaskNode;
	TaskNode.NodeType = TEXT("BT_Task");
	TaskNode.NodeName = TEXT("Wait");
	TaskNode.NodeClass = TEXT("BTTask_Wait");
	TaskNode.Properties = TEXT("{\"execution_index\":1}");
	Payload.Nodes.Add(TaskNode);

	FIndexedVariable BlackboardKey;
	BlackboardKey.VarName = TEXT("TargetActor");
	BlackboardKey.VarType = TEXT("BlackboardKeyType_Object");
	BlackboardKey.Category = TEXT("Blackboard");
	BlackboardKey.DefaultValue = TEXT("CombatBlackboard");
	Payload.Variables.Add(BlackboardKey);

	MonolithSimpleArtifactSerialization::FGraphPayloadConnection Connection;
	Connection.SourceNodeIndex = 0;
	Connection.SourcePin = TEXT("Child");
	Connection.TargetNodeIndex = 1;
	Connection.TargetPin = TEXT("Wait");
	Connection.PinType = TEXT("BT_Child");
	Payload.Connections.Add(Connection);

	FMonolithArtifact Artifact;
	MonolithSimpleArtifactSerialization::SerializeGraphPayload(Payload, Artifact.Payload);

	FBehaviorTreeIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow behavior tree materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("BehaviorTree")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("BehaviorTree"), AssetId);
	TestEqual(TEXT("behavior tree shadow should contain two nodes"), NodeAggregate.RowCount, 2ull);

	const FMonolithShadowVariableAggregate VariableAggregate = DB.GetShadowVariableAggregateForAsset(TEXT("BehaviorTree"), AssetId);
	TestEqual(TEXT("behavior tree shadow should contain one variable row"), VariableAggregate.RowCount, 1ull);

	const FMonolithShadowConnectionAggregate ConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(TEXT("BehaviorTree"), AssetId);
	TestEqual(TEXT("behavior tree shadow should contain one connection row"), ConnectionAggregate.RowCount, 1ull);
	TestTrue(TEXT("behavior tree connection hash sum should be non-zero"), ConnectionAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEQSShadowArtifactConnectionsTest,
	"Monolith.Index.ShadowMode.EQSArtifactWritesConnectionShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEQSShadowArtifactConnectionsTest::RunTest(const FString& Parameters)
{
	// EQS 的重点是 Option 和它挂着的 Generator / Test 之间的内部连接。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/EQS_Shadow");
	Asset.AssetName = TEXT("EQS_Shadow");
	Asset.AssetClass = TEXT("EnvQuery");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	FIndexedNode OptionNode;
	OptionNode.NodeType = TEXT("EQS_Option");
	OptionNode.NodeName = TEXT("Option_0");
	OptionNode.NodeClass = TEXT("EnvQueryOption");
	OptionNode.Properties = TEXT("{\"test_count\":1}");
	Payload.Nodes.Add(OptionNode);

	FIndexedNode GeneratorNode;
	GeneratorNode.NodeType = TEXT("EQS_Generator");
	GeneratorNode.NodeName = TEXT("EnvQueryGenerator_CurrentLocation");
	GeneratorNode.NodeClass = TEXT("EnvQueryGenerator_CurrentLocation");
	GeneratorNode.Properties = TEXT("{\"option_index\":0}");
	Payload.Nodes.Add(GeneratorNode);

	FIndexedNode TestNode;
	TestNode.NodeType = TEXT("EQS_Test");
	TestNode.NodeName = TEXT("Option_0_Test_0");
	TestNode.NodeClass = TEXT("EnvQueryTest_Distance");
	TestNode.Properties = TEXT("{\"test_index\":0}");
	Payload.Nodes.Add(TestNode);

	MonolithSimpleArtifactSerialization::FGraphPayloadConnection GeneratorConnection;
	GeneratorConnection.SourceNodeIndex = 0;
	GeneratorConnection.SourcePin = TEXT("Generator");
	GeneratorConnection.TargetNodeIndex = 1;
	GeneratorConnection.TargetPin = TEXT("Self");
	GeneratorConnection.PinType = TEXT("EQS_Generator");
	Payload.Connections.Add(GeneratorConnection);

	MonolithSimpleArtifactSerialization::FGraphPayloadConnection TestConnection;
	TestConnection.SourceNodeIndex = 0;
	TestConnection.SourcePin = TEXT("Tests");
	TestConnection.TargetNodeIndex = 2;
	TestConnection.TargetPin = TEXT("Test_0");
	TestConnection.PinType = TEXT("EQS_Test");
	Payload.Connections.Add(TestConnection);

	FMonolithArtifact Artifact;
	MonolithSimpleArtifactSerialization::SerializeGraphPayload(Payload, Artifact.Payload);

	FEQSIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow eqs materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("EQS")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("EQS"), AssetId);
	TestEqual(TEXT("eqs shadow should contain three nodes"), NodeAggregate.RowCount, 3ull);

	const FMonolithShadowConnectionAggregate ConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(TEXT("EQS"), AssetId);
	TestEqual(TEXT("eqs shadow should contain two connections"), ConnectionAggregate.RowCount, 2ull);
	TestTrue(TEXT("eqs connection hash sum should be non-zero"), ConnectionAggregate.RowHashSum != 0ull);
	return true;
}

#if WITH_STATETREE
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStateTreeShadowArtifactConnectionsTest,
	"Monolith.Index.ShadowMode.StateTreeArtifactWritesConnectionShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStateTreeShadowArtifactConnectionsTest::RunTest(const FString& Parameters)
{
	// StateTree shadow 需要同时看到状态节点、任务节点和状态转换。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/ST_Shadow");
	Asset.AssetName = TEXT("ST_Shadow");
	Asset.AssetClass = TEXT("StateTree");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	MonolithSimpleArtifactSerialization::FGraphPayload Payload;
	FIndexedNode IdleState;
	IdleState.NodeType = TEXT("ST_State");
	IdleState.NodeName = TEXT("Idle");
	IdleState.NodeClass = TEXT("FCompactStateTreeState");
	IdleState.Properties = TEXT("{\"num_tasks\":1}");
	Payload.Nodes.Add(IdleState);

	FIndexedNode CombatState;
	CombatState.NodeType = TEXT("ST_State");
	CombatState.NodeName = TEXT("Combat");
	CombatState.NodeClass = TEXT("FCompactStateTreeState");
	CombatState.Properties = TEXT("{\"num_tasks\":0}");
	Payload.Nodes.Add(CombatState);

	FIndexedNode IdleTask;
	IdleTask.NodeType = TEXT("ST_Task");
	IdleTask.NodeName = TEXT("Idle.Task.0");
	IdleTask.NodeClass = TEXT("StateTreeTask_PlayAnimation");
	IdleTask.Properties = TEXT("{\"state_name\":\"Idle\"}");
	Payload.Nodes.Add(IdleTask);

	MonolithSimpleArtifactSerialization::FGraphPayloadConnection TaskConnection;
	TaskConnection.SourceNodeIndex = 0;
	TaskConnection.SourcePin = TEXT("Tasks");
	TaskConnection.TargetNodeIndex = 2;
	TaskConnection.TargetPin = TEXT("StateTreeTask_PlayAnimation");
	TaskConnection.PinType = TEXT("ST_Task");
	Payload.Connections.Add(TaskConnection);

	MonolithSimpleArtifactSerialization::FGraphPayloadConnection TransitionConnection;
	TransitionConnection.SourceNodeIndex = 0;
	TransitionConnection.SourcePin = TEXT("Idle");
	TransitionConnection.TargetNodeIndex = 1;
	TransitionConnection.TargetPin = TEXT("Combat");
	TransitionConnection.PinType = TEXT("ST_Transition");
	Payload.Connections.Add(TransitionConnection);

	FMonolithArtifact Artifact;
	MonolithSimpleArtifactSerialization::SerializeGraphPayload(Payload, Artifact.Payload);

	FStateTreeIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow state tree materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("StateTree")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("StateTree"), AssetId);
	TestEqual(TEXT("state tree shadow should contain three nodes"), NodeAggregate.RowCount, 3ull);

	const FMonolithShadowConnectionAggregate ConnectionAggregate = DB.GetShadowConnectionAggregateForAsset(TEXT("StateTree"), AssetId);
	TestEqual(TEXT("state tree shadow should contain two connections"), ConnectionAggregate.RowCount, 2ull);
	TestTrue(TEXT("state tree connection hash sum should be non-zero"), ConnectionAggregate.RowHashSum != 0ull);
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataTableShadowArtifactRowsTest,
	"Monolith.Index.ShadowMode.DataTableArtifactWritesRowShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataTableShadowArtifactRowsTest::RunTest(const FString& Parameters)
{
	// DataTable 的核心是 row aggregate，所以这里验证 shadow rows 数量和 hash。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/DT_Shadow");
	Asset.AssetName = TEXT("DT_Shadow");
	Asset.AssetClass = TEXT("DataTable");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UDataTable* DataTable = NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient data table should be created"), DataTable);
	DataTable->RowStruct = FTableRowBase::StaticStruct();
	FTableRowBase EmptyRow;
	DataTable->AddRow(TEXT("Alpha"), EmptyRow);
	DataTable->AddRow(TEXT("Bravo"), EmptyRow);

	FDataTableIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("datatable artifact should build from transient table"), Indexer.BuildArtifact(FAssetData(), DataTable, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow datatable materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("DataTable")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowDataTableRowAggregate RowAggregate = DB.GetShadowDataTableRowAggregateForAsset(TEXT("DataTable"), AssetId);
	TestEqual(TEXT("shadow datatable aggregate should contain two rows"), RowAggregate.RowCount, 2ull);
	TestTrue(TEXT("shadow datatable hash sum should be non-zero"), RowAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshCatalogShadowArtifactRowsTest,
	"Monolith.Index.ShadowMode.MeshCatalogArtifactWritesMeshCatalogShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshCatalogShadowArtifactRowsTest::RunTest(const FString& Parameters)
{
	// MeshCatalog 的重点是单行数值快照，所以这里直接验证 shadow 聚合能看到那一行。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_CatalogShadow");
	Asset.AssetName = TEXT("SM_CatalogShadow");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	FIndexedMeshCatalogEntry Entry;
	Entry.AssetPath = TEXT("/Game/Test/SM_CatalogShadow.SM_CatalogShadow");
	Entry.BoundsX = 120.0;
	Entry.BoundsY = 60.0;
	Entry.BoundsZ = 40.0;
	Entry.BoundsMin = 40.0;
	Entry.BoundsMid = 60.0;
	Entry.BoundsMax = 120.0;
	Entry.Volume = 288000.0;
	Entry.SizeClass = TEXT("medium");
	Entry.Category = TEXT("Props.Crate");
	Entry.TriCount = 256;
	Entry.bHasCollision = true;
	Entry.LodCount = 2;
	Entry.PivotOffsetZ = 5.0;
	Entry.bDegenerate = false;

	FMonolithArtifact Artifact;
	MonolithShadowModeTestInternal::SerializeMeshCatalogArtifact(Entry, Artifact.Payload);

	FMeshCatalogIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow mesh catalog materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("MeshCatalog")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowMeshCatalogAggregate Aggregate = DB.GetShadowMeshCatalogAggregateForAsset(TEXT("MeshCatalog"), AssetId);
	TestEqual(TEXT("shadow mesh catalog aggregate should contain one row"), Aggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow mesh catalog hash sum should be non-zero"), Aggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDependencyShadowArtifactRowsTest,
	"Monolith.Index.ShadowMode.DependencyArtifactWritesDependencyShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDependencyShadowArtifactRowsTest::RunTest(const FString& Parameters)
{
	// dependency 的 artifact 这里用手工 payload 验证：
	// 重点是“稳定包路径 -> shadow dependency rows”这条闭环，而不是 AR 自己的依赖采集。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset SourceAsset;
	SourceAsset.PackagePath = TEXT("/Game/Test/BP_DepShadow");
	SourceAsset.AssetName = TEXT("BP_DepShadow");
	SourceAsset.AssetClass = TEXT("Blueprint");
	const int64 SourceAssetId = DB.InsertAsset(SourceAsset);
	TestTrue(TEXT("source asset should be inserted"), SourceAssetId > 0);

	FIndexedAsset TargetAsset;
	TargetAsset.PackagePath = TEXT("/Game/Test/SM_DepTargetShadow");
	TargetAsset.AssetName = TEXT("SM_DepTargetShadow");
	TargetAsset.AssetClass = TEXT("StaticMesh");
	TestTrue(TEXT("target asset should be inserted"), DB.InsertAsset(TargetAsset) > 0);

	MonolithSimpleArtifactSerialization::FDependencyPayload Payload;
	MonolithSimpleArtifactSerialization::FDependencyPayloadEntry Entry;
	Entry.TargetPackagePath = TargetAsset.PackagePath;
	Entry.DependencyType = TEXT("Hard");
	Payload.Dependencies.Add(Entry);

	FMonolithArtifact Artifact;
	MonolithSimpleArtifactSerialization::SerializeDependencyPayload(Payload, Artifact.Payload);

	FDependencyIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(SourceAssetId));
	TestTrue(TEXT("shadow dependency materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, SourceAssetId, TEXT("Dependency")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(SourceAssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowDependencyAggregate Aggregate = DB.GetShadowDependencyAggregateForAsset(TEXT("Dependency"), SourceAssetId);
	TestEqual(TEXT("shadow dependency aggregate should contain one row"), Aggregate.RowCount, 1ull);
	TestTrue(TEXT("shadow dependency hash sum should be non-zero"), Aggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagShadowArtifactRowsTest,
	"Monolith.Index.ShadowMode.GameplayTagsArtifactWritesTagReferenceShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagShadowArtifactRowsTest::RunTest(const FString& Parameters)
{
	// GameplayTags 的 artifact 同样先验证“稳定 tag 名 + context -> shadow rows”闭环。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_TagShadow");
	Asset.AssetName = TEXT("BP_TagShadow");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	MonolithSimpleArtifactSerialization::FTagReferencePayload Payload;
	MonolithSimpleArtifactSerialization::FTagReferencePayloadEntry FirstReference;
	FirstReference.TagName = TEXT("Gameplay.Combat.Melee");
	FirstReference.Context = TEXT("OwnedGameplayTags");
	Payload.References.Add(FirstReference);

	MonolithSimpleArtifactSerialization::FTagReferencePayloadEntry SecondReference;
	SecondReference.TagName = TEXT("Gameplay.UI.Menu");
	SecondReference.Context = TEXT("GameplayTags");
	Payload.References.Add(SecondReference);

	FMonolithArtifact Artifact;
	MonolithSimpleArtifactSerialization::SerializeTagReferencePayload(Payload, Artifact.Payload);

	FGameplayTagIndexer Indexer;
	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow gameplay tag materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("GameplayTags")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowTagReferenceAggregate Aggregate = DB.GetShadowTagReferenceAggregateForAsset(TEXT("GameplayTags"), AssetId);
	TestEqual(TEXT("shadow tag aggregate should contain two rows"), Aggregate.RowCount, 2ull);
	TestTrue(TEXT("shadow tag hash sum should be non-zero"), Aggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUserDefinedEnumShadowArtifactVariablesTest,
	"Monolith.Index.ShadowMode.UserDefinedEnumArtifactWritesVariableShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUserDefinedEnumShadowArtifactVariablesTest::RunTest(const FString& Parameters)
{
	// 用户自定义枚举会落成一个 node + 多个变量行。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/E_Shadow");
	Asset.AssetName = TEXT("E_Shadow");
	Asset.AssetClass = TEXT("UserDefinedEnum");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UUserDefinedEnum* UserDefinedEnum = NewObject<UUserDefinedEnum>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient user defined enum should be created"), UserDefinedEnum);
	TArray<TPair<FName, int64>> EnumEntries;
	EnumEntries.Add(TPair<FName, int64>(TEXT("Idle"), 0));
	EnumEntries.Add(TPair<FName, int64>(TEXT("Run"), 1));
	TestTrue(TEXT("user defined enum should accept transient entries"), UserDefinedEnum->SetEnums(EnumEntries, UEnum::ECppForm::Namespaced));

	FUserDefinedEnumIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("user defined enum artifact should build from transient enum"), Indexer.BuildArtifact(FAssetData(), UserDefinedEnum, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow user defined enum materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("UserDefinedEnum")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("UserDefinedEnum"), AssetId);
	TestEqual(TEXT("shadow node aggregate should contain one row"), NodeAggregate.RowCount, 1ull);

	const FMonolithShadowVariableAggregate VariableAggregate = DB.GetShadowVariableAggregateForAsset(TEXT("UserDefinedEnum"), AssetId);
	TestEqual(TEXT("shadow variable aggregate should contain two rows"), VariableAggregate.RowCount, 2ull);
	TestTrue(TEXT("shadow variable hash sum should be non-zero"), VariableAggregate.RowHashSum != 0ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUserDefinedStructShadowArtifactNodesTest,
	"Monolith.Index.ShadowMode.UserDefinedStructArtifactWritesNodeShadowRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUserDefinedStructShadowArtifactNodesTest::RunTest(const FString& Parameters)
{
	// 空瞬态 struct 至少应当写出主节点；变量行则可能是 0。
	const FString DbPath = MonolithShadowModeTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/S_Shadow");
	Asset.AssetName = TEXT("S_Shadow");
	Asset.AssetClass = TEXT("UserDefinedStruct");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	UUserDefinedStruct* UserDefinedStruct = NewObject<UUserDefinedStruct>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("transient user defined struct should be created"), UserDefinedStruct);

	FUserDefinedStructIndexer Indexer;
	FMonolithArtifact Artifact;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TestTrue(TEXT("user defined struct artifact should build from transient struct"), Indexer.BuildArtifact(FAssetData(), UserDefinedStruct, AssetRegistry, Artifact));

	TestTrue(TEXT("transaction should begin"), DB.BeginTransaction());
	TestTrue(TEXT("revision write should begin"), DB.BeginAssetRevisionWrite(AssetId));
	TestTrue(TEXT("shadow user defined struct materialization should succeed"), Indexer.MaterializeArtifactToShadow(Artifact, DB, AssetId, TEXT("UserDefinedStruct")));
	TestTrue(TEXT("revision write should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestTrue(TEXT("transaction should commit"), DB.CommitTransaction());

	const FMonolithShadowNodeAggregate NodeAggregate = DB.GetShadowNodeAggregateForAsset(TEXT("UserDefinedStruct"), AssetId);
	TestEqual(TEXT("shadow node aggregate should contain one row"), NodeAggregate.RowCount, 1ull);

	const FMonolithShadowVariableAggregate VariableAggregate = DB.GetShadowVariableAggregateForAsset(TEXT("UserDefinedStruct"), AssetId);
	TestEqual(TEXT("empty transient struct should not emit variable rows"), VariableAggregate.RowCount, 0ull);
	return true;
}
