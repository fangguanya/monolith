#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithIndexDatabase.h"
#include "SQLitePreparedStatement.h"

/*
 * 这组测试重点守住 revision 语义：
 * - 新 revision 在 promote 之前不能提前露出来；
 * - discard 后旧快照必须还在；
 * - 这些规则不仅对 nodes 生效，也对 actors / datatable rows / dependencies / tag refs 生效。
 */

namespace MonolithIndexDatabaseRevisionTestInternal
{
	/** 每条 revision 测试都使用独立临时数据库。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithIndexRevision"), TEXT(".db"));
	}

	/** 直接查活动 revision 下某个 tag ref 是否可见。 */
	static int32 CountActiveTagReferencesForAsset(FMonolithIndexDatabase& DB, const int64 AssetId, const FString& TagName)
	{
		FSQLiteDatabase* RawDatabase = DB.GetRawDatabase();
		if (!RawDatabase)
		{
			return -1;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(
			*RawDatabase,
			TEXT(
				"SELECT COUNT(a.id) "
				"FROM tag_references tr "
				"JOIN tags t ON t.id = tr.tag_id "
				"LEFT JOIN assets a ON tr.asset_id = a.id "
				"AND (tr.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND tr.revision_id = 0)) "
				"WHERE tr.asset_id = ? AND t.tag_name = ?;")))
		{
			return -1;
		}

		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, TagName);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return static_cast<int32>(Count);
		}

		return 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseRevisionSwitchTest,
	"Monolith.Index.Database.RevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// nodes 的最基础切换语义：旧快照可见，直到新 revision promote。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_Revisions");
	Asset.AssetName = TEXT("BP_Revisions");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedNode InitialNode;
	InitialNode.AssetId = AssetId;
	InitialNode.NodeType = TEXT("Metadata");
	InitialNode.NodeName = TEXT("Initial");
	InitialNode.NodeClass = TEXT("K2Node");
	TestTrue(TEXT("initial node should be inserted"), DB.InsertNode(InitialNode) > 0);
	TestTrue(TEXT("first revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	const TArray<FIndexedNode> InitialNodes = DB.GetNodesForAsset(AssetId);
	TestEqual(TEXT("first revision should expose one node"), InitialNodes.Num(), 1);
	TestEqual(TEXT("first revision node should be visible"), InitialNodes.Num() > 0 ? InitialNodes[0].NodeName : FString(), FString(TEXT("Initial")));

	DB.BeginTransaction();
	TestTrue(TEXT("second revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedNode PendingNode;
	PendingNode.AssetId = AssetId;
	PendingNode.NodeType = TEXT("Metadata");
	PendingNode.NodeName = TEXT("Pending");
	PendingNode.NodeClass = TEXT("K2Node");
	TestTrue(TEXT("pending node should be inserted"), DB.InsertNode(PendingNode) > 0);

	const TArray<FIndexedNode> DuringSwitchNodes = DB.GetNodesForAsset(AssetId);
	TestEqual(TEXT("before promote queries should still see previous revision"), DuringSwitchNodes.Num(), 1);
	TestEqual(TEXT("previous revision node should remain visible until promote"), DuringSwitchNodes.Num() > 0 ? DuringSwitchNodes[0].NodeName : FString(), FString(TEXT("Initial")));

	TestTrue(TEXT("second revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	const TArray<FIndexedNode> PromotedNodes = DB.GetNodesForAsset(AssetId);
	TestEqual(TEXT("after promote queries should only see the new revision"), PromotedNodes.Num(), 1);
	TestEqual(TEXT("new revision node should become visible after promote"), PromotedNodes.Num() > 0 ? PromotedNodes[0].NodeName : FString(), FString(TEXT("Pending")));
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseRevisionDiscardTest,
	"Monolith.Index.Database.RevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// discard 的语义是“放弃这次改写”，不能把旧快照一起弄丢。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_RevisionsDiscard");
	Asset.AssetName = TEXT("BP_RevisionsDiscard");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedNode StableNode;
	StableNode.AssetId = AssetId;
	StableNode.NodeType = TEXT("Metadata");
	StableNode.NodeName = TEXT("Stable");
	StableNode.NodeClass = TEXT("K2Node");
	TestTrue(TEXT("stable node should be inserted"), DB.InsertNode(StableNode) > 0);
	TestTrue(TEXT("first revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedNode BrokenNode;
	BrokenNode.AssetId = AssetId;
	BrokenNode.NodeType = TEXT("Metadata");
	BrokenNode.NodeName = TEXT("Broken");
	BrokenNode.NodeClass = TEXT("K2Node");
	TestTrue(TEXT("replacement node should be inserted"), DB.InsertNode(BrokenNode) > 0);
	DB.DiscardAssetRevisionWrite(AssetId);
	DB.CommitTransaction();

	const TArray<FIndexedNode> FinalNodes = DB.GetNodesForAsset(AssetId);
	TestEqual(TEXT("discard should preserve previous revision"), FinalNodes.Num(), 1);
	TestEqual(TEXT("stable revision should remain visible after discard"), FinalNodes.Num() > 0 ? FinalNodes[0].NodeName : FString(), FString(TEXT("Stable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseActorRevisionSwitchTest,
	"Monolith.Index.Database.ActorRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseActorRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// actors 表也必须遵守与 nodes 一样的 promote 可见性规则。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/L_RevisionSwitch");
	Asset.AssetName = TEXT("L_RevisionSwitch");
	Asset.AssetClass = TEXT("World");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first actor revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedActor InitialActor;
	InitialActor.AssetId = AssetId;
	InitialActor.ActorName = TEXT("InitialActor");
	InitialActor.ActorClass = TEXT("StaticMeshActor");
	InitialActor.ActorLabel = TEXT("Initial Actor");
	InitialActor.Transform = TEXT("{\"loc\":\"0,0,0\"}");
	InitialActor.Components = TEXT("[]");
	TestTrue(TEXT("initial actor should be inserted"), DB.InsertActor(InitialActor) > 0);
	TestTrue(TEXT("first actor revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	const TArray<FIndexedActor> InitialActors = DB.GetActorsForAsset(AssetId);
	TestEqual(TEXT("first actor revision should expose one actor"), InitialActors.Num(), 1);
	TestEqual(TEXT("first actor should be visible"), InitialActors.Num() > 0 ? InitialActors[0].ActorName : FString(), FString(TEXT("InitialActor")));

	DB.BeginTransaction();
	TestTrue(TEXT("second actor revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedActor PendingActor;
	PendingActor.AssetId = AssetId;
	PendingActor.ActorName = TEXT("PendingActor");
	PendingActor.ActorClass = TEXT("StaticMeshActor");
	PendingActor.ActorLabel = TEXT("Pending Actor");
	PendingActor.Transform = TEXT("{\"loc\":\"100,0,0\"}");
	PendingActor.Components = TEXT("[\"Mesh\"]");
	TestTrue(TEXT("pending actor should be inserted"), DB.InsertActor(PendingActor) > 0);

	const TArray<FIndexedActor> DuringSwitchActors = DB.GetActorsForAsset(AssetId);
	TestEqual(TEXT("before actor promote queries should still see previous revision"), DuringSwitchActors.Num(), 1);
	TestEqual(TEXT("previous actor should remain visible until promote"), DuringSwitchActors.Num() > 0 ? DuringSwitchActors[0].ActorName : FString(), FString(TEXT("InitialActor")));

	TestTrue(TEXT("second actor revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	const TArray<FIndexedActor> PromotedActors = DB.GetActorsForAsset(AssetId);
	TestEqual(TEXT("after actor promote queries should only see the new revision"), PromotedActors.Num(), 1);
	TestEqual(TEXT("new actor should become visible after promote"), PromotedActors.Num() > 0 ? PromotedActors[0].ActorName : FString(), FString(TEXT("PendingActor")));
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseActorRevisionDiscardTest,
	"Monolith.Index.Database.ActorRevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseActorRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// Level actor 快照 discard 后，上一版 actor 仍应保持可见。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/L_RevisionDiscard");
	Asset.AssetName = TEXT("L_RevisionDiscard");
	Asset.AssetClass = TEXT("World");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first actor revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedActor StableActor;
	StableActor.AssetId = AssetId;
	StableActor.ActorName = TEXT("StableActor");
	StableActor.ActorClass = TEXT("StaticMeshActor");
	StableActor.ActorLabel = TEXT("Stable Actor");
	StableActor.Transform = TEXT("{\"loc\":\"0,0,0\"}");
	StableActor.Components = TEXT("[]");
	TestTrue(TEXT("stable actor should be inserted"), DB.InsertActor(StableActor) > 0);
	TestTrue(TEXT("first actor revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement actor revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedActor BrokenActor;
	BrokenActor.AssetId = AssetId;
	BrokenActor.ActorName = TEXT("BrokenActor");
	BrokenActor.ActorClass = TEXT("StaticMeshActor");
	BrokenActor.ActorLabel = TEXT("Broken Actor");
	BrokenActor.Transform = TEXT("{\"loc\":\"500,0,0\"}");
	BrokenActor.Components = TEXT("[\"Broken\"]");
	TestTrue(TEXT("replacement actor should be inserted"), DB.InsertActor(BrokenActor) > 0);
	DB.DiscardAssetRevisionWrite(AssetId);
	DB.CommitTransaction();

	const TArray<FIndexedActor> FinalActors = DB.GetActorsForAsset(AssetId);
	TestEqual(TEXT("actor discard should preserve previous revision"), FinalActors.Num(), 1);
	TestEqual(TEXT("stable actor should remain visible after discard"), FinalActors.Num() > 0 ? FinalActors[0].ActorName : FString(), FString(TEXT("StableActor")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseDataTableRevisionSwitchTest,
	"Monolith.Index.Database.DataTableRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseDataTableRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// datatable_rows 也是 revision-aware 的一等表。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/DT_RevisionSwitch");
	Asset.AssetName = TEXT("DT_RevisionSwitch");
	Asset.AssetClass = TEXT("DataTable");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first datatable revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedDataTableRow InitialRow;
	InitialRow.AssetId = AssetId;
	InitialRow.RowName = TEXT("Alpha");
	InitialRow.RowData = TEXT("{\"value\":1}");
	TestTrue(TEXT("initial datatable row should be inserted"), DB.InsertDataTableRow(InitialRow) > 0);
	TestTrue(TEXT("first datatable revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	const TArray<FIndexedDataTableRow> InitialRows = DB.GetDataTableRowsForAsset(AssetId);
	TestEqual(TEXT("first datatable revision should expose one row"), InitialRows.Num(), 1);
	TestEqual(TEXT("first datatable row should be visible"), InitialRows.Num() > 0 ? InitialRows[0].RowName : FString(), FString(TEXT("Alpha")));

	DB.BeginTransaction();
	TestTrue(TEXT("second datatable revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedDataTableRow PendingRow;
	PendingRow.AssetId = AssetId;
	PendingRow.RowName = TEXT("Bravo");
	PendingRow.RowData = TEXT("{\"value\":2}");
	TestTrue(TEXT("pending datatable row should be inserted"), DB.InsertDataTableRow(PendingRow) > 0);

	const TArray<FIndexedDataTableRow> DuringSwitchRows = DB.GetDataTableRowsForAsset(AssetId);
	TestEqual(TEXT("before datatable promote queries should still see previous revision"), DuringSwitchRows.Num(), 1);
	TestEqual(TEXT("previous datatable row should remain visible until promote"), DuringSwitchRows.Num() > 0 ? DuringSwitchRows[0].RowName : FString(), FString(TEXT("Alpha")));

	TestTrue(TEXT("second datatable revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	const TArray<FIndexedDataTableRow> PromotedRows = DB.GetDataTableRowsForAsset(AssetId);
	TestEqual(TEXT("after datatable promote queries should only see the new revision"), PromotedRows.Num(), 1);
	TestEqual(TEXT("new datatable row should become visible after promote"), PromotedRows.Num() > 0 ? PromotedRows[0].RowName : FString(), FString(TEXT("Bravo")));
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseDataTableRevisionDiscardTest,
	"Monolith.Index.Database.DataTableRevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseDataTableRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// 行级快照 discard 后，旧行仍然是查询侧看到的版本。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/DT_RevisionDiscard");
	Asset.AssetName = TEXT("DT_RevisionDiscard");
	Asset.AssetClass = TEXT("DataTable");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first datatable revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedDataTableRow StableRow;
	StableRow.AssetId = AssetId;
	StableRow.RowName = TEXT("Stable");
	StableRow.RowData = TEXT("{\"value\":10}");
	TestTrue(TEXT("stable datatable row should be inserted"), DB.InsertDataTableRow(StableRow) > 0);
	TestTrue(TEXT("first datatable revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement datatable revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedDataTableRow BrokenRow;
	BrokenRow.AssetId = AssetId;
	BrokenRow.RowName = TEXT("Broken");
	BrokenRow.RowData = TEXT("{\"value\":999}");
	TestTrue(TEXT("replacement datatable row should be inserted"), DB.InsertDataTableRow(BrokenRow) > 0);
	DB.DiscardAssetRevisionWrite(AssetId);
	DB.CommitTransaction();

	const TArray<FIndexedDataTableRow> FinalRows = DB.GetDataTableRowsForAsset(AssetId);
	TestEqual(TEXT("datatable discard should preserve previous revision"), FinalRows.Num(), 1);
	TestEqual(TEXT("stable datatable row should remain visible after discard"), FinalRows.Num() > 0 ? FinalRows[0].RowName : FString(), FString(TEXT("Stable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseMeshCatalogRevisionSwitchTest,
	"Monolith.Index.Database.MeshCatalogRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseMeshCatalogRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// mesh_catalog 也必须遵守“promote 前仍看到旧快照”的规则。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_MeshCatalogRevisionSwitch");
	Asset.AssetName = TEXT("SM_MeshCatalogRevisionSwitch");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first mesh catalog revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedMeshCatalogEntry InitialEntry;
	InitialEntry.AssetId = AssetId;
	InitialEntry.AssetPath = TEXT("/Game/Test/SM_MeshCatalogRevisionSwitch.SM_MeshCatalogRevisionSwitch");
	InitialEntry.BoundsX = 100.0;
	InitialEntry.BoundsY = 50.0;
	InitialEntry.BoundsZ = 25.0;
	InitialEntry.BoundsMin = 25.0;
	InitialEntry.BoundsMid = 50.0;
	InitialEntry.BoundsMax = 100.0;
	InitialEntry.Volume = 125000.0;
	InitialEntry.SizeClass = TEXT("medium");
	InitialEntry.Category = TEXT("Props.Crate");
	InitialEntry.TriCount = 100;
	TestTrue(TEXT("initial mesh catalog row should be inserted"), DB.InsertMeshCatalogEntry(InitialEntry) > 0);
	TestTrue(TEXT("first mesh catalog revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	const TOptional<FIndexedMeshCatalogEntry> VisibleInitialEntry = DB.GetMeshCatalogEntryForAsset(AssetId);
	TestTrue(TEXT("initial mesh catalog row should be visible"), VisibleInitialEntry.IsSet());
	TestEqual(TEXT("initial mesh catalog tri count should match"), VisibleInitialEntry.IsSet() ? VisibleInitialEntry->TriCount : -1, 100);

	DB.BeginTransaction();
	TestTrue(TEXT("second mesh catalog revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedMeshCatalogEntry PendingEntry = InitialEntry;
	PendingEntry.AssetId = AssetId;
	PendingEntry.TriCount = 250;
	PendingEntry.LodCount = 2;
	TestTrue(TEXT("pending mesh catalog row should be inserted"), DB.InsertMeshCatalogEntry(PendingEntry) > 0);

	const TOptional<FIndexedMeshCatalogEntry> DuringSwitchEntry = DB.GetMeshCatalogEntryForAsset(AssetId);
	TestTrue(TEXT("old mesh catalog row should remain visible before promote"), DuringSwitchEntry.IsSet());
	TestEqual(TEXT("old mesh catalog tri count should remain visible until promote"), DuringSwitchEntry.IsSet() ? DuringSwitchEntry->TriCount : -1, 100);

	TestTrue(TEXT("second mesh catalog revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	const TOptional<FIndexedMeshCatalogEntry> PromotedEntry = DB.GetMeshCatalogEntryForAsset(AssetId);
	TestTrue(TEXT("new mesh catalog row should be visible after promote"), PromotedEntry.IsSet());
	TestEqual(TEXT("new mesh catalog tri count should become visible after promote"), PromotedEntry.IsSet() ? PromotedEntry->TriCount : -1, 250);
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseMeshCatalogRevisionDiscardTest,
	"Monolith.Index.Database.MeshCatalogRevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseMeshCatalogRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// discard 后，旧 mesh catalog 快照不能被临时新版本冲掉。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_MeshCatalogRevisionDiscard");
	Asset.AssetName = TEXT("SM_MeshCatalogRevisionDiscard");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first mesh catalog revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedMeshCatalogEntry StableEntry;
	StableEntry.AssetId = AssetId;
	StableEntry.AssetPath = TEXT("/Game/Test/SM_MeshCatalogRevisionDiscard.SM_MeshCatalogRevisionDiscard");
	StableEntry.BoundsX = 90.0;
	StableEntry.BoundsY = 45.0;
	StableEntry.BoundsZ = 30.0;
	StableEntry.BoundsMin = 30.0;
	StableEntry.BoundsMid = 45.0;
	StableEntry.BoundsMax = 90.0;
	StableEntry.Volume = 121500.0;
	StableEntry.SizeClass = TEXT("medium");
	StableEntry.Category = TEXT("Props.Barrel");
	StableEntry.TriCount = 80;
	TestTrue(TEXT("stable mesh catalog row should be inserted"), DB.InsertMeshCatalogEntry(StableEntry) > 0);
	TestTrue(TEXT("first mesh catalog revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement mesh catalog revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedMeshCatalogEntry BrokenEntry = StableEntry;
	BrokenEntry.AssetId = AssetId;
	BrokenEntry.TriCount = 999;
	BrokenEntry.LodCount = 7;
	TestTrue(TEXT("replacement mesh catalog row should be inserted"), DB.InsertMeshCatalogEntry(BrokenEntry) > 0);
	DB.DiscardAssetRevisionWrite(AssetId);
	DB.CommitTransaction();

	const TOptional<FIndexedMeshCatalogEntry> FinalEntry = DB.GetMeshCatalogEntryForAsset(AssetId);
	TestTrue(TEXT("stable mesh catalog row should remain visible after discard"), FinalEntry.IsSet());
	TestEqual(TEXT("stable mesh catalog tri count should remain after discard"), FinalEntry.IsSet() ? FinalEntry->TriCount : -1, 80);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseDependencyRevisionSwitchTest,
	"Monolith.Index.Database.DependencyRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseDependencyRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// dependencies 迁到 revision 语义后，也不能提前泄露 pending 版本。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset SourceAsset;
	SourceAsset.PackagePath = TEXT("/Game/Test/BP_DepSource");
	SourceAsset.AssetName = TEXT("BP_DepSource");
	SourceAsset.AssetClass = TEXT("Blueprint");
	const int64 SourceAssetId = DB.InsertAsset(SourceAsset);
	TestTrue(TEXT("source asset should be inserted"), SourceAssetId > 0);

	FIndexedAsset FirstTargetAsset;
	FirstTargetAsset.PackagePath = TEXT("/Game/Test/SM_DepTargetA");
	FirstTargetAsset.AssetName = TEXT("SM_DepTargetA");
	FirstTargetAsset.AssetClass = TEXT("StaticMesh");
	const int64 FirstTargetAssetId = DB.InsertAsset(FirstTargetAsset);
	TestTrue(TEXT("first target asset should be inserted"), FirstTargetAssetId > 0);

	FIndexedAsset SecondTargetAsset;
	SecondTargetAsset.PackagePath = TEXT("/Game/Test/SM_DepTargetB");
	SecondTargetAsset.AssetName = TEXT("SM_DepTargetB");
	SecondTargetAsset.AssetClass = TEXT("StaticMesh");
	const int64 SecondTargetAssetId = DB.InsertAsset(SecondTargetAsset);
	TestTrue(TEXT("second target asset should be inserted"), SecondTargetAssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first dependency revision should begin"), DB.BeginAssetRevisionWrite(SourceAssetId));
	FIndexedDependency InitialDependency;
	InitialDependency.SourceAssetId = SourceAssetId;
	InitialDependency.TargetAssetId = FirstTargetAssetId;
	InitialDependency.DependencyType = TEXT("Hard");
	TestTrue(TEXT("initial dependency should be inserted"), DB.InsertDependency(InitialDependency) > 0);
	TestTrue(TEXT("first dependency revision should commit"), DB.CommitAssetRevisionWrite(SourceAssetId));
	DB.CommitTransaction();

	const TArray<FIndexedDependency> InitialDependencies = DB.GetDependenciesForAsset(SourceAssetId);
	TestEqual(TEXT("first dependency revision should expose one edge"), InitialDependencies.Num(), 1);
	TestEqual(TEXT("first dependency target should be visible"), InitialDependencies.Num() > 0 ? InitialDependencies[0].TargetAssetId : -1ll, FirstTargetAssetId);

	DB.BeginTransaction();
	TestTrue(TEXT("second dependency revision should begin"), DB.BeginAssetRevisionWrite(SourceAssetId));
	FIndexedDependency PendingDependency;
	PendingDependency.SourceAssetId = SourceAssetId;
	PendingDependency.TargetAssetId = SecondTargetAssetId;
	PendingDependency.DependencyType = TEXT("Soft");
	TestTrue(TEXT("pending dependency should be inserted"), DB.InsertDependency(PendingDependency) > 0);

	const TArray<FIndexedDependency> DuringSwitchDependencies = DB.GetDependenciesForAsset(SourceAssetId);
	TestEqual(TEXT("before dependency promote queries should still see previous revision"), DuringSwitchDependencies.Num(), 1);
	TestEqual(TEXT("previous dependency target should remain visible until promote"), DuringSwitchDependencies.Num() > 0 ? DuringSwitchDependencies[0].TargetAssetId : -1ll, FirstTargetAssetId);

	TestTrue(TEXT("second dependency revision should commit"), DB.CommitAssetRevisionWrite(SourceAssetId));
	const TArray<FIndexedDependency> PromotedDependencies = DB.GetDependenciesForAsset(SourceAssetId);
	TestEqual(TEXT("after dependency promote queries should only see the new revision"), PromotedDependencies.Num(), 1);
	TestEqual(TEXT("new dependency target should become visible after promote"), PromotedDependencies.Num() > 0 ? PromotedDependencies[0].TargetAssetId : -1ll, SecondTargetAssetId);
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseDependencyRevisionDiscardTest,
	"Monolith.Index.Database.DependencyRevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseDependencyRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// dependency discard 保护的是“旧依赖边”。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset SourceAsset;
	SourceAsset.PackagePath = TEXT("/Game/Test/BP_DepDiscard");
	SourceAsset.AssetName = TEXT("BP_DepDiscard");
	SourceAsset.AssetClass = TEXT("Blueprint");
	const int64 SourceAssetId = DB.InsertAsset(SourceAsset);
	TestTrue(TEXT("source asset should be inserted"), SourceAssetId > 0);

	FIndexedAsset StableTargetAsset;
	StableTargetAsset.PackagePath = TEXT("/Game/Test/SM_DepStable");
	StableTargetAsset.AssetName = TEXT("SM_DepStable");
	StableTargetAsset.AssetClass = TEXT("StaticMesh");
	const int64 StableTargetAssetId = DB.InsertAsset(StableTargetAsset);
	TestTrue(TEXT("stable target asset should be inserted"), StableTargetAssetId > 0);

	FIndexedAsset BrokenTargetAsset;
	BrokenTargetAsset.PackagePath = TEXT("/Game/Test/SM_DepBroken");
	BrokenTargetAsset.AssetName = TEXT("SM_DepBroken");
	BrokenTargetAsset.AssetClass = TEXT("StaticMesh");
	const int64 BrokenTargetAssetId = DB.InsertAsset(BrokenTargetAsset);
	TestTrue(TEXT("broken target asset should be inserted"), BrokenTargetAssetId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first dependency revision should begin"), DB.BeginAssetRevisionWrite(SourceAssetId));
	FIndexedDependency StableDependency;
	StableDependency.SourceAssetId = SourceAssetId;
	StableDependency.TargetAssetId = StableTargetAssetId;
	StableDependency.DependencyType = TEXT("Hard");
	TestTrue(TEXT("stable dependency should be inserted"), DB.InsertDependency(StableDependency) > 0);
	TestTrue(TEXT("first dependency revision should commit"), DB.CommitAssetRevisionWrite(SourceAssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement dependency revision should begin"), DB.BeginAssetRevisionWrite(SourceAssetId));
	FIndexedDependency BrokenDependency;
	BrokenDependency.SourceAssetId = SourceAssetId;
	BrokenDependency.TargetAssetId = BrokenTargetAssetId;
	BrokenDependency.DependencyType = TEXT("Soft");
	TestTrue(TEXT("replacement dependency should be inserted"), DB.InsertDependency(BrokenDependency) > 0);
	DB.DiscardAssetRevisionWrite(SourceAssetId);
	DB.CommitTransaction();

	const TArray<FIndexedDependency> FinalDependencies = DB.GetDependenciesForAsset(SourceAssetId);
	TestEqual(TEXT("dependency discard should preserve previous revision"), FinalDependencies.Num(), 1);
	TestEqual(TEXT("stable dependency target should remain visible after discard"), FinalDependencies.Num() > 0 ? FinalDependencies[0].TargetAssetId : -1ll, StableTargetAssetId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseGameplayTagReferenceRevisionSwitchTest,
	"Monolith.Index.Database.GameplayTagReferenceRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseGameplayTagReferenceRevisionSwitchTest::RunTest(const FString& Parameters)
{
	// tag_references 只能暴露 active revision 的引用，pending 行必须隐藏。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_TagSwitch");
	Asset.AssetName = TEXT("BP_TagSwitch");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	const int64 FirstTagId = DB.GetOrCreateTag(TEXT("Gameplay.TagA"), FString());
	const int64 SecondTagId = DB.GetOrCreateTag(TEXT("Gameplay.TagB"), FString());
	TestTrue(TEXT("first tag should be created"), FirstTagId > 0);
	TestTrue(TEXT("second tag should be created"), SecondTagId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first gameplay tag revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedTagReference FirstTagReference;
	FirstTagReference.TagId = FirstTagId;
	FirstTagReference.AssetId = AssetId;
	FirstTagReference.Context = TEXT("OwnedGameplayTags");
	TestTrue(TEXT("first gameplay tag reference should be inserted"), DB.InsertTagReference(FirstTagReference) > 0);
	TestTrue(TEXT("first gameplay tag revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	TestEqual(TEXT("first active tag ref should be visible"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagA")), 1);
	TestEqual(TEXT("second active tag ref should not be visible yet"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagB")), 0);

	DB.BeginTransaction();
	TestTrue(TEXT("second gameplay tag revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedTagReference SecondTagReference;
	SecondTagReference.TagId = SecondTagId;
	SecondTagReference.AssetId = AssetId;
	SecondTagReference.Context = TEXT("OwnedGameplayTags");
	TestTrue(TEXT("pending gameplay tag reference should be inserted"), DB.InsertTagReference(SecondTagReference) > 0);

	TestEqual(TEXT("old active gameplay tag ref should remain visible until promote"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagA")), 1);
	TestEqual(TEXT("pending gameplay tag ref should stay hidden until promote"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagB")), 0);

	TestTrue(TEXT("second gameplay tag revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	TestEqual(TEXT("old gameplay tag ref should disappear after promote"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagA")), 0);
	TestEqual(TEXT("new gameplay tag ref should become visible after promote"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.TagB")), 1);
	DB.CommitTransaction();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseGameplayTagReferenceRevisionDiscardTest,
	"Monolith.Index.Database.GameplayTagReferenceRevisionDiscardPreservesPreviousSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseGameplayTagReferenceRevisionDiscardTest::RunTest(const FString& Parameters)
{
	// tag reference 的 discard 也必须守住“旧引用继续可见”。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/BP_TagDiscard");
	Asset.AssetName = TEXT("BP_TagDiscard");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	const int64 StableTagId = DB.GetOrCreateTag(TEXT("Gameplay.Stable"), FString());
	const int64 BrokenTagId = DB.GetOrCreateTag(TEXT("Gameplay.Broken"), FString());
	TestTrue(TEXT("stable tag should be created"), StableTagId > 0);
	TestTrue(TEXT("broken tag should be created"), BrokenTagId > 0);

	DB.BeginTransaction();
	TestTrue(TEXT("first gameplay tag revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedTagReference StableReference;
	StableReference.TagId = StableTagId;
	StableReference.AssetId = AssetId;
	StableReference.Context = TEXT("OwnedGameplayTags");
	TestTrue(TEXT("stable gameplay tag reference should be inserted"), DB.InsertTagReference(StableReference) > 0);
	TestTrue(TEXT("first gameplay tag revision should commit"), DB.CommitAssetRevisionWrite(AssetId));
	DB.CommitTransaction();

	DB.BeginTransaction();
	TestTrue(TEXT("replacement gameplay tag revision should begin"), DB.BeginAssetRevisionWrite(AssetId));
	FIndexedTagReference BrokenReference;
	BrokenReference.TagId = BrokenTagId;
	BrokenReference.AssetId = AssetId;
	BrokenReference.Context = TEXT("OwnedGameplayTags");
	TestTrue(TEXT("replacement gameplay tag reference should be inserted"), DB.InsertTagReference(BrokenReference) > 0);
	DB.DiscardAssetRevisionWrite(AssetId);
	DB.CommitTransaction();

	TestEqual(TEXT("stable gameplay tag ref should remain visible after discard"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.Stable")), 1);
	TestEqual(TEXT("broken gameplay tag ref should stay hidden after discard"), MonolithIndexDatabaseRevisionTestInternal::CountActiveTagReferencesForAsset(DB, AssetId, TEXT("Gameplay.Broken")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseSentinelClearTablesTest,
	"Monolith.Index.Database.SentinelScopedRebuildClearsGlobalTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseSentinelClearTablesTest::RunTest(const FString& Parameters)
{
	// 旧 sentinel scoped rebuild 还会清全局表，所以这里守住 clear 语义。
	const FString DbPath = MonolithIndexDatabaseRevisionTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset SourceAsset;
	SourceAsset.PackagePath = TEXT("/Game/Test/BP_Source");
	SourceAsset.AssetName = TEXT("BP_Source");
	SourceAsset.AssetClass = TEXT("Blueprint");
	const int64 SourceAssetId = DB.InsertAsset(SourceAsset);
	TestTrue(TEXT("source asset should be inserted"), SourceAssetId > 0);

	FIndexedAsset TargetAsset;
	TargetAsset.PackagePath = TEXT("/Game/Test/SM_Target");
	TargetAsset.AssetName = TEXT("SM_Target");
	TargetAsset.AssetClass = TEXT("StaticMesh");
	const int64 TargetAssetId = DB.InsertAsset(TargetAsset);
	TestTrue(TEXT("target asset should be inserted"), TargetAssetId > 0);

	FIndexedDependency Dependency;
	Dependency.SourceAssetId = SourceAssetId;
	Dependency.TargetAssetId = TargetAssetId;
	Dependency.DependencyType = TEXT("Hard");
	TestTrue(TEXT("dependency row should be inserted"), DB.InsertDependency(Dependency) > 0);

	const int64 TagId = DB.GetOrCreateTag(TEXT("Gameplay.Test"), FString());
	TestTrue(TEXT("tag should be created"), TagId > 0);

	FIndexedTagReference TagReference;
	TagReference.TagId = TagId;
	TagReference.AssetId = SourceAssetId;
	TagReference.Context = TEXT("OwnedGameplayTags");
	TestTrue(TEXT("tag reference should be inserted"), DB.InsertTagReference(TagReference) > 0);

	TestEqual(TEXT("dependency should be queryable before clear"), DB.GetDependenciesForAsset(SourceAssetId).Num(), 1);
	TestTrue(TEXT("dependency table should clear"), DB.ClearDependencies());
	TestEqual(TEXT("dependency should be gone after clear"), DB.GetDependenciesForAsset(SourceAssetId).Num(), 0);

	TestTrue(TEXT("gameplay tag tables should clear"), DB.ClearGameplayTagIndex());
	const TSharedPtr<FJsonObject> Stats = DB.GetStats();
	TestTrue(TEXT("stats should remain available after clear"), Stats.IsValid());
	TestEqual(TEXT("tag count should be zero after clear"), Stats.IsValid() ? static_cast<int32>(Stats->GetNumberField(TEXT("tags"))) : -1, 0);
	return true;
}
