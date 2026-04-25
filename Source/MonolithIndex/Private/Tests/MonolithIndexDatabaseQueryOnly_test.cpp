#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithIndexDatabase.h"
#include "SQLitePreparedStatement.h"

/*
 * 这条测试专门守住 query-only 查询连接的契约：
 * - 能打开现有数据库；
 * - 会强制 query_only=ON；
 * - 会强制 journal_mode=DELETE；
 * - 打开后真实查询仍然可用。
 *
 * 这正是 project.* 查询路径依赖的那条连接语义。
 */

namespace MonolithIndexDatabaseQueryOnlyTestInternal
{
	/** 每次测试都使用独立临时库，避免碰真实 ProjectIndex.db。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithIndexQueryOnly"), TEXT(".db"));
	}

	/** 读取单值 PRAGMA，避免每个断言都重复手搓 statement。 */
	static FString ReadSingleTextPragma(FSQLiteDatabase& Database, const TCHAR* Sql)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Database, Sql))
		{
			return FString();
		}

		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return FString();
		}

		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}

	/** 读取单值整型 PRAGMA。 */
	static int64 ReadSingleIntPragma(FSQLiteDatabase& Database, const TCHAR* Sql)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Database, Sql))
		{
			return INDEX_NONE;
		}

		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return INDEX_NONE;
		}

		int64 Value = INDEX_NONE;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseQueryOnlyOpenTest,
	"Monolith.Index.Database.QueryOnlyOpenUsesDeleteJournalAndAllowsQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseQueryOnlyOpenTest::RunTest(const FString& Parameters)
{
	const FString DbPath = MonolithIndexDatabaseQueryOnlyTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	{
		// 先写出一份真实 schema + 数据，后面再用 query-only 方式重开同一个库。
		FMonolithIndexDatabase WriterDb;
		TestTrue(TEXT("writer database should open"), WriterDb.Open(DbPath));

		FIndexedAsset Asset;
		Asset.PackagePath = TEXT("/Game/Test/SM_QueryOnly");
		Asset.AssetName = TEXT("SM_QueryOnly");
		Asset.AssetClass = TEXT("StaticMesh");
		TestTrue(TEXT("writer should insert one asset"), WriterDb.InsertAsset(Asset) > 0);

		WriterDb.Close();
	}

	FMonolithIndexDatabase QueryDb;
	TestTrue(TEXT("query-only database should open"), QueryDb.OpenQueryOnly(DbPath));

	FSQLiteDatabase* RawDatabase = QueryDb.GetRawDatabase();
	TestNotNull(TEXT("query-only database should expose a raw SQLite handle"), RawDatabase);
	if (!RawDatabase)
	{
		return false;
	}

	const int64 QueryOnlyFlag =
		MonolithIndexDatabaseQueryOnlyTestInternal::ReadSingleIntPragma(*RawDatabase, TEXT("PRAGMA query_only;"));
	TestEqual(TEXT("query_only pragma should be enabled"), QueryOnlyFlag, static_cast<int64>(1));

	const FString JournalMode =
		MonolithIndexDatabaseQueryOnlyTestInternal::ReadSingleTextPragma(*RawDatabase, TEXT("PRAGMA journal_mode;"));
	TestTrue(
		TEXT("journal mode should be forced to DELETE"),
		JournalMode.Equals(TEXT("delete"), ESearchCase::IgnoreCase));

	const TArray<FIndexedAsset> Assets = QueryDb.FindByType(TEXT("StaticMesh"), 10, 0);
	TestEqual(TEXT("query-only connection should still return indexed assets"), Assets.Num(), 1);
	if (Assets.Num() == 1)
	{
		TestEqual(TEXT("query-only query should return the inserted asset path"), Assets[0].PackagePath, FString(TEXT("/Game/Test/SM_QueryOnly")));
	}

	return true;
}
