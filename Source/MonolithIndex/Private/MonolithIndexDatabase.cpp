#include "MonolithIndexDatabase.h"
#include "MonolithIndexerShadowMode.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * 这份实现文件是真正把 SQLite 当“本地快照数据库”来操作的地方。
 *
 * 读它时可以抓住三条主线：
 * 1. 建表和迁移：
 *    数据库第一次打开时，会在这里创建所有表；
 *    老版本数据库升级时，也是在这里补列、补索引、补 schema_version。
 * 2. revision 可见性：
 *    某个资产写新 revision 时，先把新行写进去，但 current_revision_id 不立刻切；
 *    promote 成功后再切指针，所以查询始终能读到一份完整快照。
 * 3. shadow / diff 支持：
 *    为 artifact shadow mode 提供专门的 shadow_* 表和聚合查询，
 *    让我们可以比较“生产结果”和“新链路结果”是否一致。
 *
 * 这类文件最容易变得吓人，因为 SQL 很多。
 * 一个简单的阅读办法是：
 * - 先看表结构；
 * - 再看 Insert / Get 一对一的读写函数；
 * - 最后看 revision / shadow 这些带状态切换的函数。
 */

DEFINE_LOG_CATEGORY(LogMonolithIndex);

namespace MonolithIndexDatabaseInternal
{
	static FString CompactSqlForLog(const FString& SQL, const int32 MaxLen = 220)
	{
		FString Compact = SQL;
		Compact.ReplaceInline(TEXT("\r"), TEXT(" "));
		Compact.ReplaceInline(TEXT("\n"), TEXT(" "));
		Compact.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Compact.Contains(TEXT("  ")))
		{
			Compact.ReplaceInline(TEXT("  "), TEXT(" "));
		}

		Compact.TrimStartAndEndInline();
		if (Compact.Len() > MaxLen)
		{
			return Compact.Left(MaxLen) + TEXT("...");
		}

		return Compact;
	}

	static bool TableExists(FSQLiteDatabase& Database, const FString& TableName)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;"), ESQLitePreparedStatementFlags::Persistent))
		{
			return false;
		}

		Stmt.SetBindingValueByIndex(1, TableName);
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	}

	static bool TableHasColumn(FSQLiteDatabase& Database, const TCHAR* TableName, const TCHAR* ColumnName)
	{
		FSQLitePreparedStatement Stmt;
		const FString Sql = FString::Printf(TEXT("PRAGMA table_info(%s);"), TableName);
		if (!Stmt.Create(Database, *Sql, ESQLitePreparedStatementFlags::Persistent))
		{
			return false;
		}

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString ExistingColumn;
			Stmt.GetColumnValueByIndex(1, ExistingColumn);
			if (ExistingColumn == ColumnName)
			{
				return true;
			}
		}

		return false;
	}

	static FString RowHashToHex(const uint64 Hash)
	{
		return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(Hash));
	}

	static uint64 ParseRowHashHex(const FString& HashHex)
	{
		return FCString::Strtoui64(*HashHex, nullptr, 16);
	}

	/*
	 * shadow Level 2 需要反复读取“某资产当前可见 revision”的 shadow 行。
	 * 这里把那段固定 SQL 收口成一个 helper，避免每张表各写一份 join / active-revision 条件。
	 */
	template<typename RowType, typename ReadRowFn>
	static TArray<RowType> QueryActiveShadowRowsForAsset(
		FSQLiteDatabase* Database,
		const FString& TableName,
		const int64 AssetId,
		const FString& SelectColumns,
		const FString& OrderByClause,
		ReadRowFn&& ReadRow)
	{
		TArray<RowType> Rows;
		if (!Database || AssetId <= 0 || !TableExists(*Database, TableName))
		{
			return Rows;
		}

		FString Sql = FString::Printf(
			TEXT("SELECT %s FROM %s s ")
			TEXT("JOIN assets a ON a.id = s.asset_id ")
			TEXT("WHERE s.asset_id = ? ")
			TEXT("AND (s.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND s.revision_id = 0))"),
			*SelectColumns,
			*TableName);
		if (!OrderByClause.IsEmpty())
		{
			Sql += TEXT(" ORDER BY ");
			Sql += OrderByClause;
		}
		Sql += TEXT(";");

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, *Sql))
		{
			return Rows;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			RowType Row;
			ReadRow(Stmt, Row);
			Rows.Add(MoveTemp(Row));
		}

		return Rows;
	}
}

// ============================================================
// Full table creation SQL
// ============================================================
static const TCHAR* GCreateTablesSQL =
TEXT(R"SQL(

-- Core asset table: every indexed asset
CREATE TABLE IF NOT EXISTS assets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    package_path TEXT NOT NULL UNIQUE,
    asset_name TEXT NOT NULL,
    asset_class TEXT NOT NULL,
    module_name TEXT DEFAULT '',
    description TEXT DEFAULT '',
    file_size_bytes INTEGER DEFAULT 0,
    last_modified TEXT DEFAULT '',
    saved_hash TEXT DEFAULT '',
    indexed_at TEXT DEFAULT (datetime('now')),
    current_revision_id INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_assets_class ON assets(asset_class);
CREATE INDEX IF NOT EXISTS idx_assets_name ON assets(asset_name);

-- Graph nodes (Blueprint nodes, Material expressions, Niagara modules, etc.)
CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    node_type TEXT NOT NULL,
    node_name TEXT NOT NULL,
    node_class TEXT DEFAULT '',
    properties TEXT DEFAULT '{}',
    pos_x INTEGER DEFAULT 0,
    pos_y INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_nodes_asset ON nodes(asset_id);
CREATE INDEX IF NOT EXISTS idx_nodes_class ON nodes(node_class);

-- Pin connections between nodes
CREATE TABLE IF NOT EXISTS connections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    source_pin TEXT NOT NULL,
    target_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    target_pin TEXT NOT NULL,
    pin_type TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_conn_source ON connections(source_node_id);
CREATE INDEX IF NOT EXISTS idx_conn_target ON connections(target_node_id);

-- Variables (Blueprint variables, material parameters, niagara parameters)
CREATE TABLE IF NOT EXISTS variables (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    var_name TEXT NOT NULL,
    var_type TEXT NOT NULL,
    category TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    is_exposed INTEGER DEFAULT 0,
    is_replicated INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_vars_asset ON variables(asset_id);

-- Parameters (Material params, Niagara params, etc.)
CREATE TABLE IF NOT EXISTS parameters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    param_name TEXT NOT NULL,
    param_type TEXT NOT NULL,
    param_group TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    source TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_params_asset ON parameters(asset_id);

-- Asset dependency graph
CREATE TABLE IF NOT EXISTS dependencies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    target_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    dependency_type TEXT DEFAULT 'Hard'
);
CREATE INDEX IF NOT EXISTS idx_dep_source ON dependencies(source_asset_id);
CREATE INDEX IF NOT EXISTS idx_dep_target ON dependencies(target_asset_id);

-- Level actors
CREATE TABLE IF NOT EXISTS actors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    actor_name TEXT NOT NULL,
    actor_class TEXT NOT NULL,
    actor_label TEXT DEFAULT '',
    transform TEXT DEFAULT '{}',
    components TEXT DEFAULT '[]'
);
CREATE INDEX IF NOT EXISTS idx_actors_asset ON actors(asset_id);
CREATE INDEX IF NOT EXISTS idx_actors_class ON actors(actor_class);

-- Gameplay tags
CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_name TEXT NOT NULL UNIQUE,
    parent_tag TEXT DEFAULT '',
    reference_count INTEGER DEFAULT 0
);

-- Tag references (which assets use which tags)
CREATE TABLE IF NOT EXISTS tag_references (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    context TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_tagref_tag ON tag_references(tag_id);
CREATE INDEX IF NOT EXISTS idx_tagref_asset ON tag_references(asset_id);

-- Config/INI entries
CREATE TABLE IF NOT EXISTS configs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    section TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_configs_file ON configs(file_path);

-- C++ symbols (from tree-sitter via MonolithSource)
CREATE TABLE IF NOT EXISTS cpp_symbols (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    symbol_name TEXT NOT NULL,
    symbol_type TEXT NOT NULL,
    signature TEXT DEFAULT '',
    line_number INTEGER DEFAULT 0,
    parent_symbol TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_cpp_file ON cpp_symbols(file_path);
CREATE INDEX IF NOT EXISTS idx_cpp_name ON cpp_symbols(symbol_name);

-- Data table rows
CREATE TABLE IF NOT EXISTS datatable_rows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    row_name TEXT NOT NULL,
    row_data TEXT DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_dt_asset ON datatable_rows(asset_id);

-- StaticMesh range-query catalog
CREATE TABLE IF NOT EXISTS mesh_catalog (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    asset_path TEXT NOT NULL,
    bounds_x REAL DEFAULT 0,
    bounds_y REAL DEFAULT 0,
    bounds_z REAL DEFAULT 0,
    bounds_min REAL DEFAULT 0,
    bounds_mid REAL DEFAULT 0,
    bounds_max REAL DEFAULT 0,
    volume REAL DEFAULT 0,
    size_class TEXT DEFAULT '',
    category TEXT DEFAULT '',
    tri_count INTEGER DEFAULT 0,
    has_collision INTEGER DEFAULT 0,
    lod_count INTEGER DEFAULT 0,
    pivot_offset_z REAL DEFAULT 0,
    degenerate INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_mesh_catalog_asset ON mesh_catalog(asset_id);
CREATE INDEX IF NOT EXISTS idx_mesh_catalog_asset_path ON mesh_catalog(asset_path);
CREATE INDEX IF NOT EXISTS idx_mesh_catalog_sorted_dims ON mesh_catalog(bounds_min, bounds_mid, bounds_max);
CREATE INDEX IF NOT EXISTS idx_mesh_catalog_category ON mesh_catalog(category);
CREATE INDEX IF NOT EXISTS idx_mesh_catalog_size_class ON mesh_catalog(size_class);

-- Per-asset index metadata used for stale/version semantics
CREATE TABLE IF NOT EXISTS asset_index_metadata (
    asset_id INTEGER PRIMARY KEY REFERENCES assets(id) ON DELETE CASCADE,
    indexer_id TEXT NOT NULL,
    indexer_version INTEGER DEFAULT 1,
    artifact_schema_version INTEGER DEFAULT 1,
    identity_provider TEXT DEFAULT '',
    execution_mode TEXT DEFAULT '',
    identity_hash TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_asset_index_metadata_indexer ON asset_index_metadata(indexer_id);

-- Shadow table retention registry
CREATE TABLE IF NOT EXISTS shadow_table_retention (
    table_name TEXT PRIMARY KEY,
    cohort_name TEXT NOT NULL,
    base_table_name TEXT NOT NULL,
    expires_at_utc TEXT NOT NULL,
    rollback_retained INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_shadow_table_retention_expiry ON shadow_table_retention(expires_at_utc);
)SQL")

TEXT(R"SQL(
-- FTS5 index over assets (name, class, description, path, module)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_assets USING fts5(
    asset_name,
    asset_class,
    description,
    package_path,
    module_name,
    content=assets,
    content_rowid=id,
    tokenize='porter unicode61'
);

-- FTS5 triggers to keep index in sync
CREATE TRIGGER IF NOT EXISTS fts_assets_ai AFTER INSERT ON assets BEGIN
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_ad AFTER DELETE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_au AFTER UPDATE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;
)SQL")

TEXT(R"SQL(
-- FTS5 index over nodes (name, class, type)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_nodes USING fts5(
    node_name,
    node_class,
    node_type,
    content=nodes,
    content_rowid=id,
    tokenize='porter unicode61'
);

CREATE TRIGGER IF NOT EXISTS fts_nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;

-- Metadata table for tracking index state
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT DEFAULT ''
);

-- AssetVisualGeometric cohort: 64-dim FP32 embedding 行存储
-- 设计：双 cohort 共用 schema，按 CohortName 分别落到 asset_visual_geometric / asset_visual_semantic
-- 物理上是两张完全独立的表，互不影响 stale / shadow / reducer
CREATE TABLE IF NOT EXISTS asset_visual_geometric (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    asset_path TEXT NOT NULL,
    shard_id TEXT NOT NULL DEFAULT '',
    shard_prefix_depth INTEGER DEFAULT 0,
    provider_id TEXT NOT NULL DEFAULT '',
    provider_version INTEGER DEFAULT 1,
    render_recipe_version INTEGER DEFAULT 1,
    embedding_dim INTEGER DEFAULT 0,
    embedding_dtype INTEGER DEFAULT 0,
    embedding_bytes BLOB,
    preview_view_path TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_asset_visual_geometric_asset ON asset_visual_geometric(asset_id);
CREATE INDEX IF NOT EXISTS idx_asset_visual_geometric_shard ON asset_visual_geometric(shard_id);

CREATE TABLE IF NOT EXISTS asset_visual_semantic (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    revision_id INTEGER DEFAULT 0,
    asset_path TEXT NOT NULL,
    shard_id TEXT NOT NULL DEFAULT '',
    shard_prefix_depth INTEGER DEFAULT 0,
    provider_id TEXT NOT NULL DEFAULT '',
    provider_version INTEGER DEFAULT 1,
    render_recipe_version INTEGER DEFAULT 1,
    embedding_dim INTEGER DEFAULT 0,
    embedding_dtype INTEGER DEFAULT 0,
    embedding_bytes BLOB,
    preview_view_path TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_asset_visual_semantic_asset ON asset_visual_semantic(asset_id);
CREATE INDEX IF NOT EXISTS idx_asset_visual_semantic_shard ON asset_visual_semantic(shard_id);

)SQL");

// ============================================================
// Constructor / Destructor
// ============================================================

FMonolithIndexDatabase::FMonolithIndexDatabase()
{
}

FMonolithIndexDatabase::~FMonolithIndexDatabase()
{
	Close();
}

bool FMonolithIndexDatabase::Open(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;

	// Ensure directory exists
	FString Dir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode — WAL + ReadOnly on Windows silently returns 0 rows.
	// Belt-and-suspenders: force DELETE here regardless of what the DB was created with.
	ExecuteSQL(TEXT("PRAGMA journal_mode=DELETE;"));
	ExecuteSQL(TEXT("PRAGMA synchronous=NORMAL;"));
	ExecuteSQL(TEXT("PRAGMA foreign_keys=ON;"));
	ExecuteSQL(TEXT("PRAGMA cache_size=-64000;")); // 64MB cache

	if (!CreateTables())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create index tables"));
		Close();
		return false;
	}

	// Schema migration: v1+ -> current
	{
		FString SchemaVersion = ReadMeta(TEXT("schema_version"));
		int32 SchemaVersionInt = SchemaVersion.IsEmpty() ? 0 : FCString::Atoi(*SchemaVersion);
		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("Index DB schema check: path=%s current_schema_version=%d"),
			*DbPath,
			SchemaVersionInt);
		if (SchemaVersionInt < 4)
		{
			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("assets"), TEXT("saved_hash")))
			{
				ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN saved_hash TEXT DEFAULT '';"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("assets"), TEXT("current_revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN current_revision_id INTEGER DEFAULT 0;"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("nodes"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE nodes ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("variables"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE variables ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("parameters"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE parameters ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("actors"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE actors ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			ExecuteSQL(
				TEXT("CREATE TABLE IF NOT EXISTS shadow_table_retention (")
				TEXT("table_name TEXT PRIMARY KEY, ")
				TEXT("cohort_name TEXT NOT NULL, ")
				TEXT("base_table_name TEXT NOT NULL, ")
				TEXT("expires_at_utc TEXT NOT NULL, ")
				TEXT("rollback_retained INTEGER DEFAULT 0);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_shadow_table_retention_expiry ON shadow_table_retention(expires_at_utc);"));

			WriteMeta(TEXT("schema_version"), TEXT("4"));
			SchemaVersionInt = 4;
		}

		if (SchemaVersionInt < 5)
		{
			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("datatable_rows"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE datatable_rows ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			WriteMeta(TEXT("schema_version"), TEXT("5"));
			SchemaVersionInt = 5;
		}

		if (SchemaVersionInt < 6)
		{
			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("dependencies"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE dependencies ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			if (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("tag_references"), TEXT("revision_id")))
			{
				ExecuteSQL(TEXT("ALTER TABLE tag_references ADD COLUMN revision_id INTEGER DEFAULT 0;"));
			}

			WriteMeta(TEXT("schema_version"), TEXT("6"));
			SchemaVersionInt = 6;
		}

		if (SchemaVersionInt < 7)
		{
			/*
			 * mesh_catalog 是纯派生快照，不是权威源数据。
			 * 所以这里不做复杂迁移，而是直接把旧的无 revision 版本替换成新 schema。
			 * 这样可以避免继续背着一套旧结构兼容分支。
			 */
			if (MonolithIndexDatabaseInternal::TableExists(*Database, TEXT("mesh_catalog"))
				&& (!MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("mesh_catalog"), TEXT("asset_id"))
					|| !MonolithIndexDatabaseInternal::TableHasColumn(*Database, TEXT("mesh_catalog"), TEXT("revision_id"))))
			{
				ExecuteSQL(TEXT("DROP TABLE IF EXISTS mesh_catalog;"));
			}

			ExecuteSQL(TEXT(
				"CREATE TABLE IF NOT EXISTS mesh_catalog ("
				"    id INTEGER PRIMARY KEY AUTOINCREMENT,"
				"    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,"
				"    revision_id INTEGER DEFAULT 0,"
				"    asset_path TEXT NOT NULL,"
				"    bounds_x REAL DEFAULT 0,"
				"    bounds_y REAL DEFAULT 0,"
				"    bounds_z REAL DEFAULT 0,"
				"    bounds_min REAL DEFAULT 0,"
				"    bounds_mid REAL DEFAULT 0,"
				"    bounds_max REAL DEFAULT 0,"
				"    volume REAL DEFAULT 0,"
				"    size_class TEXT DEFAULT '',"
				"    category TEXT DEFAULT '',"
				"    tri_count INTEGER DEFAULT 0,"
				"    has_collision INTEGER DEFAULT 0,"
				"    lod_count INTEGER DEFAULT 0,"
				"    pivot_offset_z REAL DEFAULT 0,"
				"    degenerate INTEGER DEFAULT 0"
				");"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_catalog_asset ON mesh_catalog(asset_id);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_catalog_asset_path ON mesh_catalog(asset_path);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_catalog_sorted_dims ON mesh_catalog(bounds_min, bounds_mid, bounds_max);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_catalog_category ON mesh_catalog(category);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_catalog_size_class ON mesh_catalog(size_class);"));

			WriteMeta(TEXT("schema_version"), TEXT("7"));
		}

		// v7 -> v8：AssetVisual 双 cohort 表（geometric + semantic）
		// 与 mesh_catalog 同样属于 companion，但承担视觉检索而不是结构化检索
		if (SchemaVersionInt < 8)
		{
			const TCHAR* const VisualTables[] = {
				TEXT("asset_visual_geometric"),
				TEXT("asset_visual_semantic"),
			};
			for (const TCHAR* TableName : VisualTables)
			{
				const FString CreateSql = FString::Printf(
					TEXT("CREATE TABLE IF NOT EXISTS %s ("
						"    id INTEGER PRIMARY KEY AUTOINCREMENT,"
						"    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,"
						"    revision_id INTEGER DEFAULT 0,"
						"    asset_path TEXT NOT NULL,"
						"    shard_id TEXT NOT NULL DEFAULT '',"
						"    shard_prefix_depth INTEGER DEFAULT 0,"
						"    provider_id TEXT NOT NULL DEFAULT '',"
						"    provider_version INTEGER DEFAULT 1,"
						"    render_recipe_version INTEGER DEFAULT 1,"
						"    embedding_dim INTEGER DEFAULT 0,"
						"    embedding_dtype INTEGER DEFAULT 0,"
						"    embedding_bytes BLOB,"
						"    preview_view_path TEXT DEFAULT ''"
						");"),
					TableName);
				ExecuteSQL(*CreateSql);
				ExecuteSQL(*FString::Printf(TEXT("CREATE INDEX IF NOT EXISTS idx_%s_asset ON %s(asset_id);"), TableName, TableName));
				ExecuteSQL(*FString::Printf(TEXT("CREATE INDEX IF NOT EXISTS idx_%s_shard ON %s(shard_id);"), TableName, TableName));
			}
			WriteMeta(TEXT("schema_version"), TEXT("8"));
		}

		// v8 -> v9：MeshVisual* → AssetVisual* 重命名后清理 v8 早期写入的 idx_mesh_visual_* 索引名，
		// 同时让 AssetVisual* cohort 行的 provider_id / 旧 indexer 标识全量 stale，
		// 强制 4 类资产（StaticMesh / SkeletalMesh / Material / WidgetBlueprint）走一次重建。
		if (SchemaVersionInt < 9)
		{
			ExecuteSQL(TEXT("DROP INDEX IF EXISTS idx_mesh_visual_geometric_asset;"));
			ExecuteSQL(TEXT("DROP INDEX IF EXISTS idx_mesh_visual_geometric_shard;"));
			ExecuteSQL(TEXT("DROP INDEX IF EXISTS idx_mesh_visual_semantic_asset;"));
			ExecuteSQL(TEXT("DROP INDEX IF EXISTS idx_mesh_visual_semantic_shard;"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_asset_visual_geometric_asset ON asset_visual_geometric(asset_id);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_asset_visual_geometric_shard ON asset_visual_geometric(shard_id);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_asset_visual_semantic_asset ON asset_visual_semantic(asset_id);"));
			ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_asset_visual_semantic_shard ON asset_visual_semantic(shard_id);"));

			// 清空两张视觉表内容：渲染 recipe v1→v2 不兼容，5 类资产支持范围扩展，
			// 已有 StaticMesh-only 行的 embedding 与新批次空间不齐，必须整库重建。
			ExecuteSQL(TEXT("DELETE FROM asset_visual_geometric;"));
			ExecuteSQL(TEXT("DELETE FROM asset_visual_semantic;"));

			// 让 asset_index_metadata 中两个 visual cohort 的 ArtifactId 失效，
			// 触发 commandlet / live 路径上的 stale 判定 → 重投 OfflineOnly 队列。
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'AssetVisualGeometric';"));
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'AssetVisualSemantic';"));
			// 把任何遗留的旧 IndexerId 行也删掉（早期 dev DB 上可能写过 MeshVisual* 名字）。
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'MeshVisualGeometric';"));
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'MeshVisualSemantic';"));

			WriteMeta(TEXT("schema_version"), TEXT("9"));
		}

		// v9 -> v10：RenderRecipeVersion 由 2 升到 3（capture mode 改成 SCS_BaseColor 修
		// commandlet 模式下整图全黑、Geometric embedding 全 0 的 bug）。所有像素值都变了，
		// 之前 24007+24007 行 embedding 全部失效，必须清空让下一次 warmup 重建。
		if (SchemaVersionInt < 10)
		{
			ExecuteSQL(TEXT("DELETE FROM asset_visual_geometric;"));
			ExecuteSQL(TEXT("DELETE FROM asset_visual_semantic;"));
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'AssetVisualGeometric';"));
			ExecuteSQL(TEXT("DELETE FROM asset_index_metadata WHERE indexer_id = 'AssetVisualSemantic';"));
			WriteMeta(TEXT("schema_version"), TEXT("10"));
		}
	}

	// Ensure hash index exists (safe for both fresh and migrated DBs)
	ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));

	UE_LOG(LogMonolithIndex, Log, TEXT("Index database opened: %s"), *DbPath);
	return true;
}

bool FMonolithIndexDatabase::OpenQueryOnly(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;
	Database = new FSQLiteDatabase();

	/*
	 * 这里绝不能再用 SQLite 的 ReadOnly 打开模式。
	 *
	 * Monolith 自带的离线查询工具已经验证过：
	 * - Windows 上在 writer 持锁窗口里用 ReadOnly 新开连接；
	 * - 很容易在 sqlite3_open 阶段直接报 disk I/O / SQLITE_BUSY；
	 * - 即使库文件本身完全健康，也会把 project.* 查询打崩。
	 *
	 * 所以这里和 monolith_query 保持完全一致，保证编辑器内查询链和离线查询链不再各走各的：
	 * - 先用 ReadWrite 打开现有数据库；
	 * - 再把连接降成 query_only；
	 * - 同时强制 journal_mode=DELETE，避免只读/WAL 组合在 Windows 上的异常行为。
	 *
	 * 下面这段重试仍然保留，因为 writer 切换事务的瞬间依然可能让 open 失败；
	 * 但现在失败窗口会小很多，而且打开成功后连接就能长期复用，不会每次查询都重新撞锁。
	 */
	constexpr int32 MaxOpenAttempts = 30;
	constexpr float OpenRetrySeconds = 0.1f;
	bool bOpened = false;
	for (int32 Attempt = 0; Attempt < MaxOpenAttempts; ++Attempt)
	{
		if (Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
		{
			bOpened = true;
			if (Attempt > 0)
			{
				UE_LOG(
					LogMonolithIndex,
					Verbose,
					TEXT("Index database opened query-only after %d retr%s: %s"),
					Attempt,
					Attempt == 1 ? TEXT("y") : TEXT("ies"),
					*DbPath);
			}
			break;
		}
		if (Attempt + 1 < MaxOpenAttempts)
		{
			FPlatformProcess::Sleep(OpenRetrySeconds);
		}
	}

	if (!bOpened)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database query-only after retries: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	/*
	 * 这三个 PRAGMA 是查询连接的固定配置，缺一不可：
	 * - journal_mode=DELETE：避免 Windows 上 WAL + 只读/查询连接的异常读结果；
	 * - query_only=ON：把这个连接彻底锁成“只能查，不能写”；
	 * - busy_timeout=5000：单条 statement 命中 writer 时，交给 SQLite 自己排队等待。
	 *
	 * 它们必须在打开成功之后再设。
	 */
	if (!ExecuteSQL(TEXT("PRAGMA journal_mode=DELETE;"), TEXT("OpenQueryOnly/JournalMode"))
		|| !ExecuteSQL(TEXT("PRAGMA query_only=ON;"), TEXT("OpenQueryOnly/QueryOnly"))
		|| !ExecuteSQL(TEXT("PRAGMA busy_timeout=5000;"), TEXT("OpenQueryOnly/BusyTimeout")))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to configure query-only SQLite pragmas for %s"), *DbPath);
		Close();
		return false;
	}

	UE_LOG(LogMonolithIndex, Verbose, TEXT("Index database opened query-only: %s"), *DbPath);
	return true;
}

void FMonolithIndexDatabase::Close()
{
	ActiveAssetRevisions.Reset();
	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}
}

bool FMonolithIndexDatabase::IsOpen() const
{
	return Database != nullptr && Database->IsValid();
}

bool FMonolithIndexDatabase::ResetDatabase()
{
	if (!IsOpen()) return false;
	ActiveAssetRevisions.Reset();
	bool bResetSucceeded = true;

	UE_LOG(LogMonolithIndex, Log, TEXT("ResetDatabase: rebuilding schema for %s"), *DbPath);

	// Drop all tables and recreate — order matters for foreign keys
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ai;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ad;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_au;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ai;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ad;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_au;"), TEXT("ResetDatabase/DropTrigger"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_assets;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_nodes;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS tag_references;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS tags;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS connections;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS nodes;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS variables;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS parameters;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS dependencies;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS actors;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS configs;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS cpp_symbols;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS datatable_rows;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS mesh_catalog;"), TEXT("ResetDatabase/DropTable"));
	{
		TArray<FString> ShadowTableNames;
		{
			FSQLitePreparedStatement ShadowTableStmt;
			if (!ShadowTableStmt.Create(*Database, TEXT("SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE 'shadow_%';")))
			{
				UE_LOG(
					LogMonolithIndex,
					Error,
					TEXT("ResetDatabase: failed to enumerate shadow tables for %s: %s"),
					*DbPath,
					*Database->GetLastError());
				bResetSucceeded = false;
			}
			while (ShadowTableStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString ShadowTableName;
				ShadowTableStmt.GetColumnValueByIndex(0, ShadowTableName);
				ShadowTableNames.Add(MoveTemp(ShadowTableName));
			}
		}

		// 先结束 sqlite_master 的扫描，再修改 schema，避免 SQLite 因为同连接下的 schema 读写冲突而锁表。
		for (const FString& ShadowTableName : ShadowTableNames)
		{
			UE_LOG(
				LogMonolithIndex,
				Log,
				TEXT("ResetDatabase: dropping shadow table %s from %s"),
				*ShadowTableName,
				*DbPath);
			bResetSucceeded &= ExecuteSQL(
				FString::Printf(TEXT("DROP TABLE IF EXISTS %s;"), *ShadowTableName),
				TEXT("ResetDatabase/DropShadowTable"));
		}
	}
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS meta;"), TEXT("ResetDatabase/DropTable"));
	bResetSucceeded &= ExecuteSQL(TEXT("DROP TABLE IF EXISTS assets;"), TEXT("ResetDatabase/DropTable"));

	return bResetSucceeded && CreateTables();
}

// ============================================================
// Transaction helpers
// ============================================================

bool FMonolithIndexDatabase::BeginTransaction()
{
	return ExecuteSQL(TEXT("BEGIN TRANSACTION;"));
}

bool FMonolithIndexDatabase::CommitTransaction()
{
	return ExecuteSQL(TEXT("COMMIT;"));
}

bool FMonolithIndexDatabase::RollbackTransaction()
{
	ActiveAssetRevisions.Reset();
	return ExecuteSQL(TEXT("ROLLBACK;"));
}

bool FMonolithIndexDatabase::BeginAssetRevisionWrite(const int64 AssetId)
{
	if (!IsOpen() || AssetId <= 0)
	{
		return false;
	}

	if (ActiveAssetRevisions.Contains(AssetId))
	{
		return true;
	}

	int64 CurrentRevisionId = 0;
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT current_revision_id FROM assets WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByIndex(0, CurrentRevisionId);
	}

	ActiveAssetRevisions.Add(AssetId, FMath::Max<int64>(1, CurrentRevisionId + 1));
	return true;
}

bool FMonolithIndexDatabase::CommitAssetRevisionWrite(const int64 AssetId)
{
	const int64* RevisionId = ActiveAssetRevisions.Find(AssetId);
	if (!RevisionId)
	{
		return false;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET current_revision_id = ? WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, *RevisionId);
	Stmt.SetBindingValueByIndex(2, AssetId);
	const bool bUpdated = Stmt.Execute();
	const bool bDeleted = DeleteSupersededAssetRevisionRows(AssetId, *RevisionId);
	ActiveAssetRevisions.Remove(AssetId);
	return bUpdated && bDeleted;
}

void FMonolithIndexDatabase::DiscardAssetRevisionWrite(const int64 AssetId)
{
	const int64* RevisionId = ActiveAssetRevisions.Find(AssetId);
	if (!RevisionId)
	{
		return;
	}

	auto DeleteRows = [this, AssetId, RevisionId](const TCHAR* TableName)
	{
		FSQLitePreparedStatement DeleteStmt;
		const FString Sql = FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), TableName);
		DeleteStmt.Create(*Database, *Sql);
		DeleteStmt.SetBindingValueByIndex(1, AssetId);
		DeleteStmt.SetBindingValueByIndex(2, *RevisionId);
		DeleteStmt.Execute();
	};

	DeleteRows(TEXT("parameters"));
	DeleteRows(TEXT("variables"));
	DeleteRows(TEXT("nodes"));
	DeleteRows(TEXT("actors"));
	DeleteRows(TEXT("datatable_rows"));
	DeleteRows(TEXT("mesh_catalog"));
	DeleteRows(TEXT("tag_references"));

	{
		FSQLitePreparedStatement DeleteDependencyRowsStmt;
		DeleteDependencyRowsStmt.Create(*Database, TEXT("DELETE FROM dependencies WHERE source_asset_id = ? AND revision_id = ?;"));
		DeleteDependencyRowsStmt.SetBindingValueByIndex(1, AssetId);
		DeleteDependencyRowsStmt.SetBindingValueByIndex(2, *RevisionId);
		DeleteDependencyRowsStmt.Execute();
	}

	auto DeleteShadowRowsMatching = [this, &DeleteRows](const TCHAR* Pattern)
	{
		FSQLitePreparedStatement ShadowTableStmt;
		ShadowTableStmt.Create(*Database, TEXT("SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE ?;"));
		ShadowTableStmt.SetBindingValueByIndex(1, Pattern);
		while (ShadowTableStmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString ShadowTableName;
			ShadowTableStmt.GetColumnValueByIndex(0, ShadowTableName);
			DeleteRows(*ShadowTableName);
		}
	};

	DeleteShadowRowsMatching(TEXT("shadow_%_nodes"));
	DeleteShadowRowsMatching(TEXT("shadow_%_variables"));
	DeleteShadowRowsMatching(TEXT("shadow_%_actors"));
	DeleteShadowRowsMatching(TEXT("shadow_%_datatable_rows"));
	DeleteShadowRowsMatching(TEXT("shadow_%_mesh_catalog"));
	DeleteShadowRowsMatching(TEXT("shadow_%_parameters"));
	DeleteShadowRowsMatching(TEXT("shadow_%_connections"));
	ActiveAssetRevisions.Remove(AssetId);
}

// ============================================================
// Asset CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertAsset(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO assets (package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, current_revision_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Asset.PackagePath);
	Stmt.SetBindingValueByIndex(2, Asset.AssetName);
	Stmt.SetBindingValueByIndex(3, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(4, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(5, Asset.Description);
	Stmt.SetBindingValueByIndex(6, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(7, Asset.LastModified);
	Stmt.SetBindingValueByIndex(8, Asset.SavedHash);
	Stmt.SetBindingValueByIndex(9, Asset.CurrentRevisionId);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedAsset> FMonolithIndexDatabase::GetAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at, current_revision_id FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Stmt.GetColumnValueByIndex(10, Asset.CurrentRevisionId);
		return Asset;
	}
	return {};
}

int64 FMonolithIndexDatabase::GetAssetId(const FString& PackagePath)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		return Id;
	}
	return -1;
}

bool FMonolithIndexDatabase::DeleteAssetAndRelated(int64 AssetId)
{
	// CASCADE handles child rows
	return ExecuteSQL(FString::Printf(TEXT("DELETE FROM assets WHERE id = %lld;"), AssetId));
}

// ============================================================
// Node CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertNode(const FIndexedNode& Node)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, revision_id, node_type, node_name, node_class, properties, pos_x, pos_y) VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Node.AssetId);
	Stmt.SetBindingValueByIndex(2, Node.RevisionId > 0 ? Node.RevisionId : ResolveActiveRevisionId(Node.AssetId));
	Stmt.SetBindingValueByIndex(3, Node.NodeType);
	Stmt.SetBindingValueByIndex(4, Node.NodeName);
	Stmt.SetBindingValueByIndex(5, Node.NodeClass);
	Stmt.SetBindingValueByIndex(6, Node.Properties);
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Node.PosX));
	Stmt.SetBindingValueByIndex(8, static_cast<int64>(Node.PosY));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedNode> FMonolithIndexDatabase::GetNodesForAsset(int64 AssetId)
{
	TArray<FIndexedNode> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT n.id, n.asset_id, n.revision_id, n.node_type, n.node_name, n.node_class, n.properties, n.pos_x, n.pos_y FROM nodes n JOIN assets a ON a.id = n.asset_id WHERE n.asset_id = ? AND (n.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND n.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedNode Node;
		Stmt.GetColumnValueByIndex(0, Node.Id);
		Stmt.GetColumnValueByIndex(1, Node.AssetId);
		Stmt.GetColumnValueByIndex(2, Node.RevisionId);
		Stmt.GetColumnValueByIndex(3, Node.NodeType);
		Stmt.GetColumnValueByIndex(4, Node.NodeName);
		Stmt.GetColumnValueByIndex(5, Node.NodeClass);
		Stmt.GetColumnValueByIndex(6, Node.Properties);
		Stmt.GetColumnValueByIndex(7, Node.PosX);
		Stmt.GetColumnValueByIndex(8, Node.PosY);
		Result.Add(MoveTemp(Node));
	}
	return Result;
}

// ============================================================
// Connection CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConnection(const FIndexedConnection& Conn)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO connections (source_node_id, source_pin, target_node_id, target_pin, pin_type) VALUES (?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Conn.SourceNodeId);
	Stmt.SetBindingValueByIndex(2, Conn.SourcePin);
	Stmt.SetBindingValueByIndex(3, Conn.TargetNodeId);
	Stmt.SetBindingValueByIndex(4, Conn.TargetPin);
	Stmt.SetBindingValueByIndex(5, Conn.PinType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedConnection> FMonolithIndexDatabase::GetConnectionsForAsset(int64 AssetId)
{
	TArray<FIndexedConnection> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT c.id, c.source_node_id, c.source_pin, c.target_node_id, c.target_pin, c.pin_type FROM connections c INNER JOIN nodes n ON c.source_node_id = n.id INNER JOIN assets a ON a.id = n.asset_id WHERE n.asset_id = ? AND (n.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND n.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedConnection Conn;
		Stmt.GetColumnValueByIndex(0, Conn.Id);
		Stmt.GetColumnValueByIndex(1, Conn.SourceNodeId);
		Stmt.GetColumnValueByIndex(2, Conn.SourcePin);
		Stmt.GetColumnValueByIndex(3, Conn.TargetNodeId);
		Stmt.GetColumnValueByIndex(4, Conn.TargetPin);
		Stmt.GetColumnValueByIndex(5, Conn.PinType);
		Result.Add(MoveTemp(Conn));
	}
	return Result;
}

TArray<FMonolithShadowIndexedConnection> FMonolithIndexDatabase::GetProductionConnectionsForAsset(const int64 AssetId)
{
	TArray<FMonolithShadowIndexedConnection> Result;
	if (!IsOpen() || AssetId <= 0)
	{
		return Result;
	}

	const TArray<FIndexedNode> Nodes = GetNodesForAsset(AssetId);
	const TArray<FIndexedConnection> Connections = GetConnectionsForAsset(AssetId);

	TMap<int64, uint64> NodeIdToRowHash;
	NodeIdToRowHash.Reserve(Nodes.Num());
	for (const FIndexedNode& Node : Nodes)
	{
		NodeIdToRowHash.Add(Node.Id, ComputeNodeRowHash(Node));
	}

	for (const FIndexedConnection& Connection : Connections)
	{
		const uint64* SourceNodeRowHash = NodeIdToRowHash.Find(Connection.SourceNodeId);
		const uint64* TargetNodeRowHash = NodeIdToRowHash.Find(Connection.TargetNodeId);
		if (!SourceNodeRowHash || !TargetNodeRowHash)
		{
			continue;
		}

		FMonolithShadowIndexedConnection ShadowConnection;
		ShadowConnection.SourceNodeRowHash = *SourceNodeRowHash;
		ShadowConnection.SourcePin = Connection.SourcePin;
		ShadowConnection.TargetNodeRowHash = *TargetNodeRowHash;
		ShadowConnection.TargetPin = Connection.TargetPin;
		ShadowConnection.PinType = Connection.PinType;
		ShadowConnection.RowHash = ComputeConnectionRowHash(
			ShadowConnection.SourceNodeRowHash,
			ShadowConnection.SourcePin,
			ShadowConnection.TargetNodeRowHash,
			ShadowConnection.TargetPin,
			ShadowConnection.PinType);
		Result.Add(MoveTemp(ShadowConnection));
	}

	return Result;
}

// ============================================================
// Variable CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertVariable(const FIndexedVariable& Var)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO variables (asset_id, revision_id, var_name, var_type, category, default_value, is_exposed, is_replicated) VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Var.AssetId);
	Stmt.SetBindingValueByIndex(2, Var.RevisionId > 0 ? Var.RevisionId : ResolveActiveRevisionId(Var.AssetId));
	Stmt.SetBindingValueByIndex(3, Var.VarName);
	Stmt.SetBindingValueByIndex(4, Var.VarType);
	Stmt.SetBindingValueByIndex(5, Var.Category);
	Stmt.SetBindingValueByIndex(6, Var.DefaultValue);
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Var.bIsExposed ? 1 : 0));
	Stmt.SetBindingValueByIndex(8, static_cast<int64>(Var.bIsReplicated ? 1 : 0));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedVariable> FMonolithIndexDatabase::GetVariablesForAsset(int64 AssetId)
{
	TArray<FIndexedVariable> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT v.id, v.asset_id, v.revision_id, v.var_name, v.var_type, v.category, v.default_value, v.is_exposed, v.is_replicated FROM variables v JOIN assets a ON a.id = v.asset_id WHERE v.asset_id = ? AND (v.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND v.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedVariable Var;
		Stmt.GetColumnValueByIndex(0, Var.Id);
		Stmt.GetColumnValueByIndex(1, Var.AssetId);
		Stmt.GetColumnValueByIndex(2, Var.RevisionId);
		Stmt.GetColumnValueByIndex(3, Var.VarName);
		Stmt.GetColumnValueByIndex(4, Var.VarType);
		Stmt.GetColumnValueByIndex(5, Var.Category);
		Stmt.GetColumnValueByIndex(6, Var.DefaultValue);
		int32 Exposed = 0, Replicated = 0;
		Stmt.GetColumnValueByIndex(7, Exposed);
		Stmt.GetColumnValueByIndex(8, Replicated);
		Var.bIsExposed = Exposed != 0;
		Var.bIsReplicated = Replicated != 0;
		Result.Add(MoveTemp(Var));
	}
	return Result;
}

// ============================================================
// Parameter CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertParameter(const FIndexedParameter& Param)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO parameters (asset_id, revision_id, param_name, param_type, param_group, default_value, source) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Param.AssetId);
	Stmt.SetBindingValueByIndex(2, Param.RevisionId > 0 ? Param.RevisionId : ResolveActiveRevisionId(Param.AssetId));
	Stmt.SetBindingValueByIndex(3, Param.ParamName);
	Stmt.SetBindingValueByIndex(4, Param.ParamType);
	Stmt.SetBindingValueByIndex(5, Param.ParamGroup);
	Stmt.SetBindingValueByIndex(6, Param.DefaultValue);
	Stmt.SetBindingValueByIndex(7, Param.Source);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedParameter> FMonolithIndexDatabase::GetParametersForAsset(int64 AssetId)
{
	TArray<FIndexedParameter> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT p.id, p.asset_id, p.revision_id, p.param_name, p.param_type, p.param_group, p.default_value, p.source FROM parameters p JOIN assets a ON a.id = p.asset_id WHERE p.asset_id = ? AND (p.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND p.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedParameter Param;
		Stmt.GetColumnValueByIndex(0, Param.Id);
		Stmt.GetColumnValueByIndex(1, Param.AssetId);
		Stmt.GetColumnValueByIndex(2, Param.RevisionId);
		Stmt.GetColumnValueByIndex(3, Param.ParamName);
		Stmt.GetColumnValueByIndex(4, Param.ParamType);
		Stmt.GetColumnValueByIndex(5, Param.ParamGroup);
		Stmt.GetColumnValueByIndex(6, Param.DefaultValue);
		Stmt.GetColumnValueByIndex(7, Param.Source);
		Result.Add(MoveTemp(Param));
	}

	return Result;
}

// ============================================================
// Dependency CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDependency(const FIndexedDependency& Dep)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO dependencies (source_asset_id, revision_id, target_asset_id, dependency_type) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Dep.SourceAssetId);
	Stmt.SetBindingValueByIndex(2, Dep.RevisionId > 0 ? Dep.RevisionId : ResolveWriteOrCurrentRevisionId(Dep.SourceAssetId));
	Stmt.SetBindingValueByIndex(3, Dep.TargetAssetId);
	Stmt.SetBindingValueByIndex(4, Dep.DependencyType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetDependenciesForAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT d.id, d.source_asset_id, d.revision_id, d.target_asset_id, d.dependency_type FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.source_asset_id = ? AND (d.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND d.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.RevisionId);
		Stmt.GetColumnValueByIndex(3, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(4, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

TArray<FMonolithShadowIndexedDependency> FMonolithIndexDatabase::GetProductionDependenciesForAsset(const int64 AssetId)
{
	TArray<FMonolithShadowIndexedDependency> Result;
	if (!IsOpen() || AssetId <= 0)
	{
		return Result;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT(
			"SELECT COALESCE(target.package_path, ''), d.dependency_type "
			"FROM dependencies d "
			"JOIN assets source ON source.id = d.source_asset_id "
			"LEFT JOIN assets target ON target.id = d.target_asset_id "
			"WHERE d.source_asset_id = ? "
			"AND (d.revision_id = source.current_revision_id OR (source.current_revision_id = 0 AND d.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithShadowIndexedDependency Row;
		Stmt.GetColumnValueByIndex(0, Row.TargetPackagePath);
		Stmt.GetColumnValueByIndex(1, Row.DependencyType);
		Row.RowHash = ComputeDependencyRowHash(Row.TargetPackagePath, Row.DependencyType);
		Result.Add(MoveTemp(Row));
	}

	return Result;
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetReferencersOfAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT d.id, d.source_asset_id, d.revision_id, d.target_asset_id, d.dependency_type FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.target_asset_id = ? AND (d.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND d.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.RevisionId);
		Stmt.GetColumnValueByIndex(3, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(4, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

bool FMonolithIndexDatabase::ClearDependencies()
{
	return ExecuteSQL(TEXT("DELETE FROM dependencies;"));
}

// ============================================================
// Actor CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertActor(const FIndexedActor& Actor)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO actors (asset_id, revision_id, actor_name, actor_class, actor_label, transform, components) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Actor.AssetId);
	Stmt.SetBindingValueByIndex(2, Actor.RevisionId > 0 ? Actor.RevisionId : ResolveActiveRevisionId(Actor.AssetId));
	Stmt.SetBindingValueByIndex(3, Actor.ActorName);
	Stmt.SetBindingValueByIndex(4, Actor.ActorClass);
	Stmt.SetBindingValueByIndex(5, Actor.ActorLabel);
	Stmt.SetBindingValueByIndex(6, Actor.Transform);
	Stmt.SetBindingValueByIndex(7, Actor.Components);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedActor> FMonolithIndexDatabase::GetActorsForAsset(int64 AssetId)
{
	TArray<FIndexedActor> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT v.id, v.asset_id, v.revision_id, v.actor_name, v.actor_class, v.actor_label, v.transform, v.components FROM actors v JOIN assets a ON a.id = v.asset_id WHERE v.asset_id = ? AND (v.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND v.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedActor Actor;
		Stmt.GetColumnValueByIndex(0, Actor.Id);
		Stmt.GetColumnValueByIndex(1, Actor.AssetId);
		Stmt.GetColumnValueByIndex(2, Actor.RevisionId);
		Stmt.GetColumnValueByIndex(3, Actor.ActorName);
		Stmt.GetColumnValueByIndex(4, Actor.ActorClass);
		Stmt.GetColumnValueByIndex(5, Actor.ActorLabel);
		Stmt.GetColumnValueByIndex(6, Actor.Transform);
		Stmt.GetColumnValueByIndex(7, Actor.Components);
		Result.Add(MoveTemp(Actor));
	}

	return Result;
}

// ============================================================
// Tag CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertTag(const FIndexedTag& Tag)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR IGNORE INTO tags (tag_name, parent_tag, reference_count) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Tag.TagName);
	Stmt.SetBindingValueByIndex(2, Tag.ParentTag);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Tag.ReferenceCount));

	if (!Stmt.Execute()) return -1;
	return GetOrCreateTag(Tag.TagName, Tag.ParentTag);
}

int64 FMonolithIndexDatabase::GetOrCreateTag(const FString& TagName, const FString& ParentTag)
{
	if (!IsOpen()) return -1;

	// Try to get existing
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM tags WHERE tag_name = ?;"));
	Stmt.SetBindingValueByIndex(1, TagName);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		if (!ParentTag.IsEmpty())
		{
			FSQLitePreparedStatement UpdateParentStmt;
			UpdateParentStmt.Create(*Database, TEXT("UPDATE tags SET parent_tag = CASE WHEN parent_tag = '' THEN ? ELSE parent_tag END WHERE id = ?;"));
			UpdateParentStmt.SetBindingValueByIndex(1, ParentTag);
			UpdateParentStmt.SetBindingValueByIndex(2, Id);
			UpdateParentStmt.Execute();
		}
		return Id;
	}

	// Insert new
	FSQLitePreparedStatement InsertStmt;
	InsertStmt.Create(*Database, TEXT("INSERT INTO tags (tag_name, parent_tag) VALUES (?, ?);"));
	InsertStmt.SetBindingValueByIndex(1, TagName);
	InsertStmt.SetBindingValueByIndex(2, ParentTag);
	InsertStmt.Execute();
	return Database->GetLastInsertRowId();
}

int64 FMonolithIndexDatabase::InsertTagReference(const FIndexedTagReference& Ref)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO tag_references (tag_id, asset_id, revision_id, context) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Ref.TagId);
	Stmt.SetBindingValueByIndex(2, Ref.AssetId);
	Stmt.SetBindingValueByIndex(3, Ref.RevisionId > 0 ? Ref.RevisionId : ResolveWriteOrCurrentRevisionId(Ref.AssetId));
	Stmt.SetBindingValueByIndex(4, Ref.Context);

	if (!Stmt.Execute()) return -1;

	// Update reference count
	FSQLitePreparedStatement UpdateStmt;
	UpdateStmt.Create(*Database, TEXT("UPDATE tags SET reference_count = (SELECT COUNT(*) FROM tag_references WHERE tag_id = ?) WHERE id = ?;"));
	UpdateStmt.SetBindingValueByIndex(1, Ref.TagId);
	UpdateStmt.SetBindingValueByIndex(2, Ref.TagId);
	UpdateStmt.Execute();

	return Database->GetLastInsertRowId();
}

TArray<FMonolithShadowIndexedTagReference> FMonolithIndexDatabase::GetProductionTagReferencesForAsset(const int64 AssetId)
{
	TArray<FMonolithShadowIndexedTagReference> Result;
	if (!IsOpen() || AssetId <= 0)
	{
		return Result;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT(
			"SELECT t.tag_name, tr.context "
			"FROM tag_references tr "
			"JOIN assets a ON a.id = tr.asset_id "
			"JOIN tags t ON t.id = tr.tag_id "
			"WHERE tr.asset_id = ? "
			"AND (tr.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND tr.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithShadowIndexedTagReference Row;
		Stmt.GetColumnValueByIndex(0, Row.TagName);
		Stmt.GetColumnValueByIndex(1, Row.Context);
		Row.RowHash = ComputeTagReferenceRowHash(Row.TagName, Row.Context);
		Result.Add(MoveTemp(Row));
	}

	return Result;
}

bool FMonolithIndexDatabase::ClearGameplayTagIndex()
{
	return ExecuteSQL(TEXT("DELETE FROM tag_references;"))
		&& ExecuteSQL(TEXT("DELETE FROM tags;"));
}

TArray<FIndexedGameplayTagSummary> FMonolithIndexDatabase::ListGameplayTags(const FString& Prefix)
{
	TArray<FIndexedGameplayTagSummary> Result;
	if (!IsOpen())
	{
		return Result;
	}

	// 这里的 active_reference_count 只统计“当前可见 revision”里的引用，
	// 这样 pending revision 里的半成品不会提前漏给查询侧。
	FSQLitePreparedStatement Stmt;
	if (Prefix.IsEmpty())
	{
		Stmt.Create(*Database, TEXT(
			"SELECT t.tag_name, t.parent_tag, COUNT(a.id) AS active_reference_count "
			"FROM tags t "
			"LEFT JOIN tag_references tr ON t.id = tr.tag_id "
			"LEFT JOIN assets a ON tr.asset_id = a.id "
			"AND (tr.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND tr.revision_id = 0)) "
			"GROUP BY t.id "
			"ORDER BY t.tag_name;"));
	}
	else
	{
		Stmt.Create(*Database, TEXT(
			"SELECT t.tag_name, t.parent_tag, COUNT(a.id) AS active_reference_count "
			"FROM tags t "
			"LEFT JOIN tag_references tr ON t.id = tr.tag_id "
			"LEFT JOIN assets a ON tr.asset_id = a.id "
			"AND (tr.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND tr.revision_id = 0)) "
			"WHERE t.tag_name LIKE ? "
			"GROUP BY t.id "
			"ORDER BY t.tag_name;"));
		Stmt.SetBindingValueByIndex(1, Prefix + TEXT("%"));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedGameplayTagSummary Summary;
		Stmt.GetColumnValueByIndex(0, Summary.TagName);
		Stmt.GetColumnValueByIndex(1, Summary.ParentTag);
		Stmt.GetColumnValueByIndex(2, Summary.ReferenceCount);
		Result.Add(MoveTemp(Summary));
	}

	return Result;
}

TArray<FIndexedGameplayTagSummary> FMonolithIndexDatabase::SearchGameplayTags(const FString& Query)
{
	TArray<FIndexedGameplayTagSummary> Result;
	if (!IsOpen())
	{
		return Result;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT(
		"SELECT t.tag_name, t.parent_tag, COUNT(a.id) AS active_reference_count, "
		"GROUP_CONCAT(a.package_path) AS referencing_assets "
		"FROM tags t "
		"LEFT JOIN tag_references tr ON t.id = tr.tag_id "
		"LEFT JOIN assets a ON tr.asset_id = a.id "
		"AND (tr.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND tr.revision_id = 0)) "
		"WHERE t.tag_name LIKE ? "
		"GROUP BY t.id "
		"ORDER BY active_reference_count DESC, t.tag_name;"));
	Stmt.SetBindingValueByIndex(1, TEXT("%") + Query + TEXT("%"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedGameplayTagSummary Summary;
		FString ReferencingAssetsRaw;
		Stmt.GetColumnValueByIndex(0, Summary.TagName);
		Stmt.GetColumnValueByIndex(1, Summary.ParentTag);
		Stmt.GetColumnValueByIndex(2, Summary.ReferenceCount);
		Stmt.GetColumnValueByIndex(3, ReferencingAssetsRaw);

		if (!ReferencingAssetsRaw.IsEmpty())
		{
			ReferencingAssetsRaw.ParseIntoArray(Summary.ReferencingAssets, TEXT(","));
			for (FString& AssetPath : Summary.ReferencingAssets)
			{
				AssetPath = AssetPath.TrimStartAndEnd();
			}
			Summary.ReferencingAssets.RemoveAll([](const FString& AssetPath)
			{
				return AssetPath.IsEmpty();
			});
		}

		Result.Add(MoveTemp(Summary));
	}

	return Result;
}

// ============================================================
// Config CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConfig(const FIndexedConfig& Config)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO configs (file_path, section, key, value) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Config.FilePath);
	Stmt.SetBindingValueByIndex(2, Config.Section);
	Stmt.SetBindingValueByIndex(3, Config.Key);
	Stmt.SetBindingValueByIndex(4, Config.Value);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

bool FMonolithIndexDatabase::ClearConfigIndex()
{
	return ExecuteSQL(TEXT("DELETE FROM configs;"));
}

// ============================================================
// C++ Symbol CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertCppSymbol(const FIndexedCppSymbol& Symbol)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO cpp_symbols (file_path, symbol_name, symbol_type, signature, line_number, parent_symbol) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Symbol.FilePath);
	Stmt.SetBindingValueByIndex(2, Symbol.SymbolName);
	Stmt.SetBindingValueByIndex(3, Symbol.SymbolType);
	Stmt.SetBindingValueByIndex(4, Symbol.Signature);
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Symbol.LineNumber));
	Stmt.SetBindingValueByIndex(6, Symbol.ParentSymbol);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

bool FMonolithIndexDatabase::ClearCppSymbolIndex()
{
	return ExecuteSQL(TEXT("DELETE FROM cpp_symbols;"));
}

// ============================================================
// DataTable Row CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDataTableRow(const FIndexedDataTableRow& Row)
{
	if (!IsOpen()) return -1;

	const int64 RevisionId = ResolveActiveRevisionId(Row.AssetId);
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO datatable_rows (asset_id, revision_id, row_name, row_data) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Row.AssetId);
	Stmt.SetBindingValueByIndex(2, RevisionId);
	Stmt.SetBindingValueByIndex(3, Row.RowName);
	Stmt.SetBindingValueByIndex(4, Row.RowData);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedDataTableRow> FMonolithIndexDatabase::GetDataTableRowsForAsset(const int64 AssetId)
{
	TArray<FIndexedDataTableRow> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT d.id, d.asset_id, d.revision_id, d.row_name, d.row_data FROM datatable_rows d JOIN assets a ON a.id = d.asset_id WHERE d.asset_id = ? AND (d.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND d.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDataTableRow Row;
		Stmt.GetColumnValueByIndex(0, Row.Id);
		Stmt.GetColumnValueByIndex(1, Row.AssetId);
		Stmt.GetColumnValueByIndex(2, Row.RevisionId);
		Stmt.GetColumnValueByIndex(3, Row.RowName);
		Stmt.GetColumnValueByIndex(4, Row.RowData);
		Result.Add(MoveTemp(Row));
	}

	return Result;
}

// ============================================================
// Mesh catalog CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertMeshCatalogEntry(const FIndexedMeshCatalogEntry& Entry)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT("INSERT INTO mesh_catalog (asset_id, revision_id, asset_path, bounds_x, bounds_y, bounds_z, bounds_min, bounds_mid, bounds_max, volume, size_class, category, tri_count, has_collision, lod_count, pivot_offset_z, degenerate) ")
		TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Entry.AssetId);
	Stmt.SetBindingValueByIndex(2, Entry.RevisionId > 0 ? Entry.RevisionId : ResolveActiveRevisionId(Entry.AssetId));
	Stmt.SetBindingValueByIndex(3, Entry.AssetPath);
	Stmt.SetBindingValueByIndex(4, Entry.BoundsX);
	Stmt.SetBindingValueByIndex(5, Entry.BoundsY);
	Stmt.SetBindingValueByIndex(6, Entry.BoundsZ);
	Stmt.SetBindingValueByIndex(7, Entry.BoundsMin);
	Stmt.SetBindingValueByIndex(8, Entry.BoundsMid);
	Stmt.SetBindingValueByIndex(9, Entry.BoundsMax);
	Stmt.SetBindingValueByIndex(10, Entry.Volume);
	Stmt.SetBindingValueByIndex(11, Entry.SizeClass);
	Stmt.SetBindingValueByIndex(12, Entry.Category);
	Stmt.SetBindingValueByIndex(13, static_cast<int64>(Entry.TriCount));
	Stmt.SetBindingValueByIndex(14, static_cast<int64>(Entry.bHasCollision ? 1 : 0));
	Stmt.SetBindingValueByIndex(15, static_cast<int64>(Entry.LodCount));
	Stmt.SetBindingValueByIndex(16, Entry.PivotOffsetZ);
	Stmt.SetBindingValueByIndex(17, static_cast<int64>(Entry.bDegenerate ? 1 : 0));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedMeshCatalogEntry> FMonolithIndexDatabase::GetMeshCatalogEntryForAsset(const int64 AssetId)
{
	if (!IsOpen())
	{
		return {};
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT("SELECT m.id, m.asset_id, m.revision_id, m.asset_path, m.bounds_x, m.bounds_y, m.bounds_z, m.bounds_min, m.bounds_mid, m.bounds_max, m.volume, m.size_class, m.category, m.tri_count, m.has_collision, m.lod_count, m.pivot_offset_z, m.degenerate ")
		TEXT("FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id ")
		TEXT("WHERE m.asset_id = ? AND (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0));"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return {};
	}

	FIndexedMeshCatalogEntry Entry;
	int32 HasCollision = 0;
	int32 Degenerate = 0;
	Stmt.GetColumnValueByIndex(0, Entry.Id);
	Stmt.GetColumnValueByIndex(1, Entry.AssetId);
	Stmt.GetColumnValueByIndex(2, Entry.RevisionId);
	Stmt.GetColumnValueByIndex(3, Entry.AssetPath);
	Stmt.GetColumnValueByIndex(4, Entry.BoundsX);
	Stmt.GetColumnValueByIndex(5, Entry.BoundsY);
	Stmt.GetColumnValueByIndex(6, Entry.BoundsZ);
	Stmt.GetColumnValueByIndex(7, Entry.BoundsMin);
	Stmt.GetColumnValueByIndex(8, Entry.BoundsMid);
	Stmt.GetColumnValueByIndex(9, Entry.BoundsMax);
	Stmt.GetColumnValueByIndex(10, Entry.Volume);
	Stmt.GetColumnValueByIndex(11, Entry.SizeClass);
	Stmt.GetColumnValueByIndex(12, Entry.Category);
	Stmt.GetColumnValueByIndex(13, Entry.TriCount);
	Stmt.GetColumnValueByIndex(14, HasCollision);
	Stmt.GetColumnValueByIndex(15, Entry.LodCount);
	Stmt.GetColumnValueByIndex(16, Entry.PivotOffsetZ);
	Stmt.GetColumnValueByIndex(17, Degenerate);
	Entry.bHasCollision = HasCollision != 0;
	Entry.bDegenerate = Degenerate != 0;
	return Entry;
}

TArray<FIndexedMeshCatalogEntry> FMonolithIndexDatabase::GetMeshCatalogEntries(const FString& PathFilter)
{
	TArray<FIndexedMeshCatalogEntry> Result;
	if (!IsOpen())
	{
		return Result;
	}

	FString Sql =
		TEXT("SELECT m.id, m.asset_id, m.revision_id, m.asset_path, m.bounds_x, m.bounds_y, m.bounds_z, m.bounds_min, m.bounds_mid, m.bounds_max, m.volume, m.size_class, m.category, m.tri_count, m.has_collision, m.lod_count, m.pivot_offset_z, m.degenerate ")
		TEXT("FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id ")
		TEXT("WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0)) ");
	if (!PathFilter.IsEmpty())
	{
		Sql += TEXT("AND m.asset_path LIKE ? ");
	}
	Sql += TEXT("ORDER BY m.asset_path ASC;");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *Sql);
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(1, FString::Printf(TEXT("%%%s%%"), *PathFilter));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedMeshCatalogEntry Entry;
		int32 HasCollision = 0;
		int32 Degenerate = 0;
		Stmt.GetColumnValueByIndex(0, Entry.Id);
		Stmt.GetColumnValueByIndex(1, Entry.AssetId);
		Stmt.GetColumnValueByIndex(2, Entry.RevisionId);
		Stmt.GetColumnValueByIndex(3, Entry.AssetPath);
		Stmt.GetColumnValueByIndex(4, Entry.BoundsX);
		Stmt.GetColumnValueByIndex(5, Entry.BoundsY);
		Stmt.GetColumnValueByIndex(6, Entry.BoundsZ);
		Stmt.GetColumnValueByIndex(7, Entry.BoundsMin);
		Stmt.GetColumnValueByIndex(8, Entry.BoundsMid);
		Stmt.GetColumnValueByIndex(9, Entry.BoundsMax);
		Stmt.GetColumnValueByIndex(10, Entry.Volume);
		Stmt.GetColumnValueByIndex(11, Entry.SizeClass);
		Stmt.GetColumnValueByIndex(12, Entry.Category);
		Stmt.GetColumnValueByIndex(13, Entry.TriCount);
		Stmt.GetColumnValueByIndex(14, HasCollision);
		Stmt.GetColumnValueByIndex(15, Entry.LodCount);
		Stmt.GetColumnValueByIndex(16, Entry.PivotOffsetZ);
		Stmt.GetColumnValueByIndex(17, Degenerate);
		Entry.bHasCollision = HasCollision != 0;
		Entry.bDegenerate = Degenerate != 0;
		Result.Add(MoveTemp(Entry));
	}

	return Result;
}

TSharedPtr<FJsonObject> FMonolithIndexDatabase::SearchMeshCatalogBySize(
	const TArray<float>& MinBounds,
	const TArray<float>& MaxBounds,
	const FString& Category,
	const FString& ExcludeSizeClass,
	const int32 Limit)
{
	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	if (!IsOpen() || MinBounds.Num() != 3 || MaxBounds.Num() != 3)
	{
		Result->SetArrayField(TEXT("results"), ResultsArray);
		Result->SetNumberField(TEXT("total"), 0);
		return Result;
	}

	double SortedMin[3] = { MinBounds[0], MinBounds[1], MinBounds[2] };
	double SortedMax[3] = { MaxBounds[0], MaxBounds[1], MaxBounds[2] };
	if (SortedMin[0] > SortedMin[1]) Swap(SortedMin[0], SortedMin[1]);
	if (SortedMin[1] > SortedMin[2]) Swap(SortedMin[1], SortedMin[2]);
	if (SortedMin[0] > SortedMin[1]) Swap(SortedMin[0], SortedMin[1]);
	if (SortedMax[0] > SortedMax[1]) Swap(SortedMax[0], SortedMax[1]);
	if (SortedMax[1] > SortedMax[2]) Swap(SortedMax[1], SortedMax[2]);
	if (SortedMax[0] > SortedMax[1]) Swap(SortedMax[0], SortedMax[1]);

	FString SQL =
		TEXT("SELECT m.asset_path, m.bounds_x, m.bounds_y, m.bounds_z, m.category, m.tri_count, m.size_class ")
		TEXT("FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id ")
		TEXT("WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0)) ")
		TEXT("AND m.degenerate = 0 ")
		TEXT("AND m.bounds_min >= ? AND m.bounds_min <= ? ")
		TEXT("AND m.bounds_mid >= ? AND m.bounds_mid <= ? ")
		TEXT("AND m.bounds_max >= ? AND m.bounds_max <= ? ");
	if (!Category.IsEmpty())
	{
		SQL += TEXT("AND m.category LIKE ? ");
	}
	if (!ExcludeSizeClass.IsEmpty())
	{
		SQL += TEXT("AND m.size_class != ? ");
	}
	SQL += TEXT("ORDER BY m.bounds_max ASC, m.tri_count ASC LIMIT ?;");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);
	int32 BindIndex = 1;
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMin[0]);
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMax[0]);
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMin[1]);
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMax[1]);
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMin[2]);
	Stmt.SetBindingValueByIndex(BindIndex++, SortedMax[2]);
	if (!Category.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIndex++, Category + TEXT("%"));
	}
	if (!ExcludeSizeClass.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIndex++, ExcludeSizeClass);
	}
	Stmt.SetBindingValueByIndex(BindIndex++, static_cast<int64>(Limit));

	int32 Total = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString AssetPath;
		double BoundsX = 0.0;
		double BoundsY = 0.0;
		double BoundsZ = 0.0;
		FString RowCategory;
		int64 TriCount = 0;
		FString SizeClass;
		Stmt.GetColumnValueByIndex(0, AssetPath);
		Stmt.GetColumnValueByIndex(1, BoundsX);
		Stmt.GetColumnValueByIndex(2, BoundsY);
		Stmt.GetColumnValueByIndex(3, BoundsZ);
		Stmt.GetColumnValueByIndex(4, RowCategory);
		Stmt.GetColumnValueByIndex(5, TriCount);
		Stmt.GetColumnValueByIndex(6, SizeClass);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		TArray<TSharedPtr<FJsonValue>> BoundsArray;
		BoundsArray.Add(MakeShared<FJsonValueNumber>(BoundsX));
		BoundsArray.Add(MakeShared<FJsonValueNumber>(BoundsY));
		BoundsArray.Add(MakeShared<FJsonValueNumber>(BoundsZ));
		Entry->SetArrayField(TEXT("bounds"), BoundsArray);
		Entry->SetStringField(TEXT("category"), RowCategory);
		Entry->SetNumberField(TEXT("tri_count"), static_cast<double>(TriCount));
		Entry->SetStringField(TEXT("size_class"), SizeClass);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		++Total;
	}

	Result->SetArrayField(TEXT("results"), ResultsArray);
	Result->SetNumberField(TEXT("total"), Total);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetMeshCatalogStats()
{
	auto Result = MakeShared<FJsonObject>();
	if (!IsOpen())
	{
		return Result;
	}

	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(
			*Database,
			TEXT("SELECT COUNT(*) FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0));"));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 TotalMeshes = 0;
			Stmt.GetColumnValueByIndex(0, TotalMeshes);
			Result->SetNumberField(TEXT("total_meshes"), static_cast<double>(TotalMeshes));
		}
	}

	{
		auto Categories = MakeShared<FJsonObject>();
		FSQLitePreparedStatement Stmt;
		Stmt.Create(
			*Database,
			TEXT("SELECT m.category, COUNT(*) FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id ")
			TEXT("WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0)) ")
			TEXT("GROUP BY m.category ORDER BY COUNT(*) DESC;"));
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Category;
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Category);
			Stmt.GetColumnValueByIndex(1, Count);
			Categories->SetNumberField(Category, static_cast<double>(Count));
		}
		Result->SetObjectField(TEXT("categories"), Categories);
	}

	{
		auto SizeDistribution = MakeShared<FJsonObject>();
		FSQLitePreparedStatement Stmt;
		Stmt.Create(
			*Database,
			TEXT("SELECT m.size_class, COUNT(*) FROM mesh_catalog m JOIN assets a ON a.id = m.asset_id ")
			TEXT("WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0)) ")
			TEXT("GROUP BY m.size_class ORDER BY COUNT(*) DESC;"));
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString SizeClass;
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, SizeClass);
			Stmt.GetColumnValueByIndex(1, Count);
			SizeDistribution->SetNumberField(SizeClass, static_cast<double>(Count));
		}
		Result->SetObjectField(TEXT("size_distribution"), SizeDistribution);
	}

	return Result;
}

bool FMonolithIndexDatabase::UpsertAssetIndexMetadata(const FMonolithAssetIndexMetadata& Metadata)
{
	if (!IsOpen() || Metadata.AssetId <= 0 || Metadata.IndexerId.IsEmpty())
	{
		return false;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT("INSERT INTO asset_index_metadata (asset_id, indexer_id, indexer_version, artifact_schema_version, identity_provider, execution_mode, identity_hash) VALUES (?, ?, ?, ?, ?, ?, ?) ")
		TEXT("ON CONFLICT(asset_id) DO UPDATE SET indexer_id = excluded.indexer_id, indexer_version = excluded.indexer_version, artifact_schema_version = excluded.artifact_schema_version, identity_provider = excluded.identity_provider, execution_mode = excluded.execution_mode, identity_hash = excluded.identity_hash;"));
	Stmt.SetBindingValueByIndex(1, Metadata.AssetId);
	Stmt.SetBindingValueByIndex(2, Metadata.IndexerId);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Metadata.IndexerVersion));
	Stmt.SetBindingValueByIndex(4, static_cast<int64>(Metadata.ArtifactSchemaVersion));
	Stmt.SetBindingValueByIndex(5, Metadata.IdentityProvider);
	Stmt.SetBindingValueByIndex(6, Metadata.ExecutionMode);
	Stmt.SetBindingValueByIndex(7, Metadata.IdentityHash);
	return Stmt.Execute();
}

TOptional<FMonolithAssetIndexMetadata> FMonolithIndexDatabase::GetAssetIndexMetadataByAssetId(const int64 AssetId)
{
	if (!IsOpen() || AssetId <= 0)
	{
		return {};
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT asset_id, indexer_id, indexer_version, artifact_schema_version, identity_provider, execution_mode, identity_hash FROM asset_index_metadata WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithAssetIndexMetadata Metadata;
		int64 IndexerVersion = 1;
		int64 ArtifactSchemaVersion = 1;
		Stmt.GetColumnValueByIndex(0, Metadata.AssetId);
		Stmt.GetColumnValueByIndex(1, Metadata.IndexerId);
		Stmt.GetColumnValueByIndex(2, IndexerVersion);
		Stmt.GetColumnValueByIndex(3, ArtifactSchemaVersion);
		Stmt.GetColumnValueByIndex(4, Metadata.IdentityProvider);
		Stmt.GetColumnValueByIndex(5, Metadata.ExecutionMode);
		Stmt.GetColumnValueByIndex(6, Metadata.IdentityHash);
		Metadata.IndexerVersion = static_cast<uint32>(IndexerVersion);
		Metadata.ArtifactSchemaVersion = static_cast<uint8>(ArtifactSchemaVersion);
		return Metadata;
	}

	return {};
}

TMap<int64, FMonolithAssetIndexMetadata> FMonolithIndexDatabase::GetAllAssetIndexMetadata()
{
	TMap<int64, FMonolithAssetIndexMetadata> Result;
	if (!IsOpen())
	{
		return Result;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT("SELECT asset_id, indexer_id, indexer_version, artifact_schema_version, identity_provider, execution_mode, identity_hash FROM asset_index_metadata;"));

	// 一次 step 循环把整张表流式读出来，避免 N 次 prepared statement 创建/拆销。
	// 33K asset 实测约 30-50ms，对比 N 次 ByAssetId 的 6-8s 是 ~150x 加速。
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithAssetIndexMetadata Metadata;
		int64 IndexerVersion = 1;
		int64 ArtifactSchemaVersion = 1;
		Stmt.GetColumnValueByIndex(0, Metadata.AssetId);
		Stmt.GetColumnValueByIndex(1, Metadata.IndexerId);
		Stmt.GetColumnValueByIndex(2, IndexerVersion);
		Stmt.GetColumnValueByIndex(3, ArtifactSchemaVersion);
		Stmt.GetColumnValueByIndex(4, Metadata.IdentityProvider);
		Stmt.GetColumnValueByIndex(5, Metadata.ExecutionMode);
		Stmt.GetColumnValueByIndex(6, Metadata.IdentityHash);
		Metadata.IndexerVersion = static_cast<uint32>(IndexerVersion);
		Metadata.ArtifactSchemaVersion = static_cast<uint8>(ArtifactSchemaVersion);
		Result.Emplace(Metadata.AssetId, MoveTemp(Metadata));
	}

	return Result;
}

// ============================================================
// Meta
// ============================================================

bool FMonolithIndexDatabase::WriteMeta(const FString& Key, const FString& Value)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	return Stmt.Execute();
}

FString FMonolithIndexDatabase::ReadMeta(const FString& Key) const
{
	if (!Database || !Database->IsValid()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT value FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
	return FString();
}

bool FMonolithIndexDatabase::HasIndexedAssetSnapshot() const
{
	if (!Database || !Database->IsValid())
	{
		return false;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT 1 FROM assets LIMIT 1;"));
	return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
}

// ============================================================
// Incremental indexing helpers
// ============================================================

TArray<FString> FMonolithIndexDatabase::GetAllIndexedPaths()
{
	TArray<FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt.GetColumnValueByIndex(0, Path);
		Result.Add(MoveTemp(Path));
	}
	return Result;
}

TArray<FIndexedAsset> FMonolithIndexDatabase::GetAllAssets()
{
	TArray<FIndexedAsset> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at, current_revision_id FROM assets;"));
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Stmt.GetColumnValueByIndex(10, Asset.CurrentRevisionId);
		Result.Add(MoveTemp(Asset));
	}

	return Result;
}

bool FMonolithIndexDatabase::EnsureShadowNodesTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("nodes"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("node_type TEXT NOT NULL, ")
		TEXT("node_name TEXT NOT NULL, ")
		TEXT("node_class TEXT DEFAULT '', ")
		TEXT("properties TEXT DEFAULT '{}', ")
		TEXT("pos_x INTEGER DEFAULT 0, ")
		TEXT("pos_y INTEGER DEFAULT 0, ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowParametersTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("parameters"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("param_name TEXT NOT NULL, ")
		TEXT("param_type TEXT NOT NULL, ")
		TEXT("param_group TEXT DEFAULT '', ")
		TEXT("default_value TEXT DEFAULT '', ")
		TEXT("source TEXT DEFAULT '', ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowVariablesTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("variables"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("var_name TEXT NOT NULL, ")
		TEXT("var_type TEXT NOT NULL, ")
		TEXT("category TEXT DEFAULT '', ")
		TEXT("default_value TEXT DEFAULT '', ")
		TEXT("is_exposed INTEGER DEFAULT 0, ")
		TEXT("is_replicated INTEGER DEFAULT 0, ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowActorsTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("actors"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("actor_name TEXT NOT NULL, ")
		TEXT("actor_class TEXT NOT NULL, ")
		TEXT("actor_label TEXT DEFAULT '', ")
		TEXT("transform TEXT DEFAULT '{}', ")
		TEXT("components TEXT DEFAULT '[]', ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowDataTableRowsTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("datatable_rows"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("row_name TEXT NOT NULL, ")
		TEXT("row_data TEXT DEFAULT '{}', ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowDependenciesTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("dependencies"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("target_package_path TEXT NOT NULL, ")
		TEXT("dependency_type TEXT NOT NULL, ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowTagReferencesTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("tag_references"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("tag_name TEXT NOT NULL, ")
		TEXT("context TEXT DEFAULT '', ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowConnectionsTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("connections"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("source_node_hash TEXT NOT NULL, ")
		TEXT("source_pin TEXT NOT NULL, ")
		TEXT("target_node_hash TEXT NOT NULL, ")
		TEXT("target_pin TEXT NOT NULL, ")
		TEXT("pin_type TEXT DEFAULT '', ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::EnsureShadowMeshCatalogTable(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("mesh_catalog"));
	const FString IndexName = FString::Printf(TEXT("idx_%s_asset"), *TableName);
	return ExecuteSQL(FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s (")
		TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
		TEXT("asset_id INTEGER NOT NULL, ")
		TEXT("revision_id INTEGER DEFAULT 0, ")
		TEXT("asset_path TEXT NOT NULL, ")
		TEXT("bounds_x REAL DEFAULT 0, ")
		TEXT("bounds_y REAL DEFAULT 0, ")
		TEXT("bounds_z REAL DEFAULT 0, ")
		TEXT("bounds_min REAL DEFAULT 0, ")
		TEXT("bounds_mid REAL DEFAULT 0, ")
		TEXT("bounds_max REAL DEFAULT 0, ")
		TEXT("volume REAL DEFAULT 0, ")
		TEXT("size_class TEXT DEFAULT '', ")
		TEXT("category TEXT DEFAULT '', ")
		TEXT("tri_count INTEGER DEFAULT 0, ")
		TEXT("has_collision INTEGER DEFAULT 0, ")
		TEXT("lod_count INTEGER DEFAULT 0, ")
		TEXT("pivot_offset_z REAL DEFAULT 0, ")
		TEXT("degenerate INTEGER DEFAULT 0, ")
		TEXT("row_hash TEXT DEFAULT ''); ")
		TEXT("CREATE INDEX IF NOT EXISTS %s ON %s(asset_id);"),
		*TableName,
		*IndexName,
		*TableName));
}

bool FMonolithIndexDatabase::ReplaceShadowNodesForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedNode>& Nodes)
{
	if (!EnsureShadowNodesTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("nodes"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedNode& ShadowNode : Nodes)
	{
		const int64 RevisionId = ShadowNode.Node.RevisionId > 0 ? ShadowNode.Node.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, node_type, node_name, node_class, properties, pos_x, pos_y, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowNode.Node.NodeType);
		InsertStmt.SetBindingValueByIndex(4, ShadowNode.Node.NodeName);
		InsertStmt.SetBindingValueByIndex(5, ShadowNode.Node.NodeClass);
		InsertStmt.SetBindingValueByIndex(6, ShadowNode.Node.Properties);
		InsertStmt.SetBindingValueByIndex(7, static_cast<int64>(ShadowNode.Node.PosX));
		InsertStmt.SetBindingValueByIndex(8, static_cast<int64>(ShadowNode.Node.PosY));
		InsertStmt.SetBindingValueByIndex(9, MonolithIndexDatabaseInternal::RowHashToHex(ShadowNode.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedNode> FMonolithIndexDatabase::GetShadowNodesForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedNode>(
		Database,
		MakeShadowTableName(CohortName, TEXT("nodes")),
		AssetId,
		TEXT("s.node_type, s.node_name, s.node_class, s.properties, s.pos_x, s.pos_y, s.row_hash"),
		TEXT("s.node_type, s.node_name, s.node_class, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedNode& Row)
		{
			Row.Node.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Node.NodeType);
			Stmt.GetColumnValueByIndex(1, Row.Node.NodeName);
			Stmt.GetColumnValueByIndex(2, Row.Node.NodeClass);
			Stmt.GetColumnValueByIndex(3, Row.Node.Properties);
			Stmt.GetColumnValueByIndex(4, Row.Node.PosX);
			Stmt.GetColumnValueByIndex(5, Row.Node.PosY);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(6, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowNodeAggregate FMonolithIndexDatabase::GetProductionNodeAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowNodeAggregate Aggregate;
	for (const FIndexedNode& Node : GetNodesForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeNodeRowHash(Node);
	}
	return Aggregate;
}

FMonolithShadowNodeAggregate FMonolithIndexDatabase::GetShadowNodeAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowNodeAggregate Aggregate;
	for (const FMonolithShadowIndexedNode& Row : GetShadowNodesForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowVariablesForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedVariable>& Variables)
{
	if (!EnsureShadowVariablesTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("variables"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedVariable& ShadowVariable : Variables)
	{
		const int64 RevisionId = ShadowVariable.Variable.RevisionId > 0 ? ShadowVariable.Variable.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, var_name, var_type, category, default_value, is_exposed, is_replicated, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowVariable.Variable.VarName);
		InsertStmt.SetBindingValueByIndex(4, ShadowVariable.Variable.VarType);
		InsertStmt.SetBindingValueByIndex(5, ShadowVariable.Variable.Category);
		InsertStmt.SetBindingValueByIndex(6, ShadowVariable.Variable.DefaultValue);
		InsertStmt.SetBindingValueByIndex(7, ShadowVariable.Variable.bIsExposed ? 1 : 0);
		InsertStmt.SetBindingValueByIndex(8, ShadowVariable.Variable.bIsReplicated ? 1 : 0);
		InsertStmt.SetBindingValueByIndex(9, MonolithIndexDatabaseInternal::RowHashToHex(ShadowVariable.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedVariable> FMonolithIndexDatabase::GetShadowVariablesForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedVariable>(
		Database,
		MakeShadowTableName(CohortName, TEXT("variables")),
		AssetId,
		TEXT("s.var_name, s.var_type, s.category, s.default_value, s.is_exposed, s.is_replicated, s.row_hash"),
		TEXT("s.var_name, s.var_type, s.category, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedVariable& Row)
		{
			Row.Variable.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Variable.VarName);
			Stmt.GetColumnValueByIndex(1, Row.Variable.VarType);
			Stmt.GetColumnValueByIndex(2, Row.Variable.Category);
			Stmt.GetColumnValueByIndex(3, Row.Variable.DefaultValue);
			int32 Exposed = 0;
			int32 Replicated = 0;
			Stmt.GetColumnValueByIndex(4, Exposed);
			Stmt.GetColumnValueByIndex(5, Replicated);
			Row.Variable.bIsExposed = Exposed != 0;
			Row.Variable.bIsReplicated = Replicated != 0;
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(6, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowVariableAggregate FMonolithIndexDatabase::GetProductionVariableAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowVariableAggregate Aggregate;
	for (const FIndexedVariable& Variable : GetVariablesForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeVariableRowHash(Variable);
	}

	return Aggregate;
}

FMonolithShadowVariableAggregate FMonolithIndexDatabase::GetShadowVariableAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowVariableAggregate Aggregate;
	for (const FMonolithShadowIndexedVariable& Row : GetShadowVariablesForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowActorsForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedActor>& Actors)
{
	if (!EnsureShadowActorsTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("actors"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedActor& ShadowActor : Actors)
	{
		const int64 RevisionId = ShadowActor.Actor.RevisionId > 0 ? ShadowActor.Actor.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, actor_name, actor_class, actor_label, transform, components, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowActor.Actor.ActorName);
		InsertStmt.SetBindingValueByIndex(4, ShadowActor.Actor.ActorClass);
		InsertStmt.SetBindingValueByIndex(5, ShadowActor.Actor.ActorLabel);
		InsertStmt.SetBindingValueByIndex(6, ShadowActor.Actor.Transform);
		InsertStmt.SetBindingValueByIndex(7, ShadowActor.Actor.Components);
		InsertStmt.SetBindingValueByIndex(8, MonolithIndexDatabaseInternal::RowHashToHex(ShadowActor.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedActor> FMonolithIndexDatabase::GetShadowActorsForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedActor>(
		Database,
		MakeShadowTableName(CohortName, TEXT("actors")),
		AssetId,
		TEXT("s.actor_name, s.actor_class, s.actor_label, s.transform, s.components, s.row_hash"),
		TEXT("s.actor_name, s.actor_class, s.actor_label, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedActor& Row)
		{
			Row.Actor.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Actor.ActorName);
			Stmt.GetColumnValueByIndex(1, Row.Actor.ActorClass);
			Stmt.GetColumnValueByIndex(2, Row.Actor.ActorLabel);
			Stmt.GetColumnValueByIndex(3, Row.Actor.Transform);
			Stmt.GetColumnValueByIndex(4, Row.Actor.Components);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(5, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowActorAggregate FMonolithIndexDatabase::GetProductionActorAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowActorAggregate Aggregate;
	for (const FIndexedActor& Actor : GetActorsForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeActorRowHash(Actor);
	}

	return Aggregate;
}

FMonolithShadowActorAggregate FMonolithIndexDatabase::GetShadowActorAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowActorAggregate Aggregate;
	for (const FMonolithShadowIndexedActor& Row : GetShadowActorsForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowDataTableRowsForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedDataTableRow>& Rows)
{
	if (!EnsureShadowDataTableRowsTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("datatable_rows"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedDataTableRow& ShadowRow : Rows)
	{
		const int64 RevisionId = ShadowRow.Row.RevisionId > 0 ? ShadowRow.Row.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, row_name, row_data, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowRow.Row.RowName);
		InsertStmt.SetBindingValueByIndex(4, ShadowRow.Row.RowData);
		InsertStmt.SetBindingValueByIndex(5, MonolithIndexDatabaseInternal::RowHashToHex(ShadowRow.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedDataTableRow> FMonolithIndexDatabase::GetShadowDataTableRowsForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedDataTableRow>(
		Database,
		MakeShadowTableName(CohortName, TEXT("datatable_rows")),
		AssetId,
		TEXT("s.row_name, s.row_data, s.row_hash"),
		TEXT("s.row_name, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedDataTableRow& Row)
		{
			Row.Row.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Row.RowName);
			Stmt.GetColumnValueByIndex(1, Row.Row.RowData);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(2, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowDataTableRowAggregate FMonolithIndexDatabase::GetProductionDataTableRowAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowDataTableRowAggregate Aggregate;
	for (const FIndexedDataTableRow& Row : GetDataTableRowsForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeDataTableRowHash(Row);
	}

	return Aggregate;
}

FMonolithShadowDataTableRowAggregate FMonolithIndexDatabase::GetShadowDataTableRowAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowDataTableRowAggregate Aggregate;
	for (const FMonolithShadowIndexedDataTableRow& Row : GetShadowDataTableRowsForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowDependenciesForAsset(
	const FString& CohortName,
	const int64 AssetId,
	const TArray<FMonolithShadowIndexedDependency>& Dependencies)
{
	if (!EnsureShadowDependenciesTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("dependencies"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedDependency& ShadowDependency : Dependencies)
	{
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, target_package_path, dependency_type, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, ActiveRevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowDependency.TargetPackagePath);
		InsertStmt.SetBindingValueByIndex(4, ShadowDependency.DependencyType);
		InsertStmt.SetBindingValueByIndex(5, MonolithIndexDatabaseInternal::RowHashToHex(ShadowDependency.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedDependency> FMonolithIndexDatabase::GetShadowDependenciesForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedDependency>(
		Database,
		MakeShadowTableName(CohortName, TEXT("dependencies")),
		AssetId,
		TEXT("s.target_package_path, s.dependency_type, s.row_hash"),
		TEXT("s.target_package_path, s.dependency_type, s.row_hash"),
		[](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedDependency& Row)
		{
			Stmt.GetColumnValueByIndex(0, Row.TargetPackagePath);
			Stmt.GetColumnValueByIndex(1, Row.DependencyType);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(2, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowDependencyAggregate FMonolithIndexDatabase::GetProductionDependencyAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowDependencyAggregate Aggregate;
	for (const FMonolithShadowIndexedDependency& Row : GetProductionDependenciesForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

FMonolithShadowDependencyAggregate FMonolithIndexDatabase::GetShadowDependencyAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowDependencyAggregate Aggregate;
	for (const FMonolithShadowIndexedDependency& Row : GetShadowDependenciesForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowTagReferencesForAsset(
	const FString& CohortName,
	const int64 AssetId,
	const TArray<FMonolithShadowIndexedTagReference>& References)
{
	if (!EnsureShadowTagReferencesTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("tag_references"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedTagReference& ShadowReference : References)
	{
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, tag_name, context, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, ActiveRevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowReference.TagName);
		InsertStmt.SetBindingValueByIndex(4, ShadowReference.Context);
		InsertStmt.SetBindingValueByIndex(5, MonolithIndexDatabaseInternal::RowHashToHex(ShadowReference.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedTagReference> FMonolithIndexDatabase::GetShadowTagReferencesForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedTagReference>(
		Database,
		MakeShadowTableName(CohortName, TEXT("tag_references")),
		AssetId,
		TEXT("s.tag_name, s.context, s.row_hash"),
		TEXT("s.tag_name, s.context, s.row_hash"),
		[](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedTagReference& Row)
		{
			Stmt.GetColumnValueByIndex(0, Row.TagName);
			Stmt.GetColumnValueByIndex(1, Row.Context);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(2, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowTagReferenceAggregate FMonolithIndexDatabase::GetProductionTagReferenceAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowTagReferenceAggregate Aggregate;
	for (const FMonolithShadowIndexedTagReference& Row : GetProductionTagReferencesForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

FMonolithShadowTagReferenceAggregate FMonolithIndexDatabase::GetShadowTagReferenceAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowTagReferenceAggregate Aggregate;
	for (const FMonolithShadowIndexedTagReference& Row : GetShadowTagReferencesForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowParametersForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedParameter>& Parameters)
{
	if (!EnsureShadowParametersTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("parameters"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedParameter& ShadowParameter : Parameters)
	{
		const int64 RevisionId = ShadowParameter.Parameter.RevisionId > 0 ? ShadowParameter.Parameter.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, param_name, param_type, param_group, default_value, source, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowParameter.Parameter.ParamName);
		InsertStmt.SetBindingValueByIndex(4, ShadowParameter.Parameter.ParamType);
		InsertStmt.SetBindingValueByIndex(5, ShadowParameter.Parameter.ParamGroup);
		InsertStmt.SetBindingValueByIndex(6, ShadowParameter.Parameter.DefaultValue);
		InsertStmt.SetBindingValueByIndex(7, ShadowParameter.Parameter.Source);
		InsertStmt.SetBindingValueByIndex(8, MonolithIndexDatabaseInternal::RowHashToHex(ShadowParameter.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedParameter> FMonolithIndexDatabase::GetShadowParametersForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedParameter>(
		Database,
		MakeShadowTableName(CohortName, TEXT("parameters")),
		AssetId,
		TEXT("s.param_name, s.param_type, s.param_group, s.default_value, s.source, s.row_hash"),
		TEXT("s.param_name, s.param_type, s.param_group, s.source, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedParameter& Row)
		{
			Row.Parameter.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Parameter.ParamName);
			Stmt.GetColumnValueByIndex(1, Row.Parameter.ParamType);
			Stmt.GetColumnValueByIndex(2, Row.Parameter.ParamGroup);
			Stmt.GetColumnValueByIndex(3, Row.Parameter.DefaultValue);
			Stmt.GetColumnValueByIndex(4, Row.Parameter.Source);
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(5, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowParameterAggregate FMonolithIndexDatabase::GetProductionParameterAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowParameterAggregate Aggregate;
	for (const FIndexedParameter& Parameter : GetParametersForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeParameterRowHash(Parameter);
	}

	return Aggregate;
}

FMonolithShadowParameterAggregate FMonolithIndexDatabase::GetShadowParameterAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowParameterAggregate Aggregate;
	for (const FMonolithShadowIndexedParameter& Row : GetShadowParametersForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowConnectionsForAsset(const FString& CohortName, const int64 AssetId, const TArray<FMonolithShadowIndexedConnection>& Connections)
{
	if (!EnsureShadowConnectionsTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("connections"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedConnection& ShadowConnection : Connections)
	{
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, source_node_hash, source_pin, target_node_hash, target_pin, pin_type, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, ActiveRevisionId);
		InsertStmt.SetBindingValueByIndex(3, MonolithIndexDatabaseInternal::RowHashToHex(ShadowConnection.SourceNodeRowHash));
		InsertStmt.SetBindingValueByIndex(4, ShadowConnection.SourcePin);
		InsertStmt.SetBindingValueByIndex(5, MonolithIndexDatabaseInternal::RowHashToHex(ShadowConnection.TargetNodeRowHash));
		InsertStmt.SetBindingValueByIndex(6, ShadowConnection.TargetPin);
		InsertStmt.SetBindingValueByIndex(7, ShadowConnection.PinType);
		InsertStmt.SetBindingValueByIndex(8, MonolithIndexDatabaseInternal::RowHashToHex(ShadowConnection.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedConnection> FMonolithIndexDatabase::GetShadowConnectionsForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedConnection>(
		Database,
		MakeShadowTableName(CohortName, TEXT("connections")),
		AssetId,
		TEXT("s.source_node_hash, s.source_pin, s.target_node_hash, s.target_pin, s.pin_type, s.row_hash"),
		TEXT("s.source_node_hash, s.source_pin, s.target_node_hash, s.target_pin, s.row_hash"),
		[](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedConnection& Row)
		{
			FString SourceNodeHashHex;
			FString TargetNodeHashHex;
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(0, SourceNodeHashHex);
			Stmt.GetColumnValueByIndex(1, Row.SourcePin);
			Stmt.GetColumnValueByIndex(2, TargetNodeHashHex);
			Stmt.GetColumnValueByIndex(3, Row.TargetPin);
			Stmt.GetColumnValueByIndex(4, Row.PinType);
			Stmt.GetColumnValueByIndex(5, RowHashHex);
			Row.SourceNodeRowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(SourceNodeHashHex);
			Row.TargetNodeRowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(TargetNodeHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowConnectionAggregate FMonolithIndexDatabase::GetProductionConnectionAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowConnectionAggregate Aggregate;
	for (const FMonolithShadowIndexedConnection& Row : GetProductionConnectionsForAsset(AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

FMonolithShadowConnectionAggregate FMonolithIndexDatabase::GetShadowConnectionAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowConnectionAggregate Aggregate;
	for (const FMonolithShadowIndexedConnection& Row : GetShadowConnectionsForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::ReplaceShadowMeshCatalogEntriesForAsset(
	const FString& CohortName,
	const int64 AssetId,
	const TArray<FMonolithShadowIndexedMeshCatalogEntry>& Entries)
{
	if (!EnsureShadowMeshCatalogTable(CohortName) || AssetId <= 0)
	{
		return false;
	}

	const FString TableName = MakeShadowTableName(CohortName, TEXT("mesh_catalog"));
	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedMeshCatalogEntry& ShadowEntry : Entries)
	{
		const int64 RevisionId = ShadowEntry.Entry.RevisionId > 0 ? ShadowEntry.Entry.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, asset_path, bounds_x, bounds_y, bounds_z, bounds_min, bounds_mid, bounds_max, volume, size_class, category, tri_count, has_collision, lod_count, pivot_offset_z, degenerate, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowEntry.Entry.AssetPath);
		InsertStmt.SetBindingValueByIndex(4, ShadowEntry.Entry.BoundsX);
		InsertStmt.SetBindingValueByIndex(5, ShadowEntry.Entry.BoundsY);
		InsertStmt.SetBindingValueByIndex(6, ShadowEntry.Entry.BoundsZ);
		InsertStmt.SetBindingValueByIndex(7, ShadowEntry.Entry.BoundsMin);
		InsertStmt.SetBindingValueByIndex(8, ShadowEntry.Entry.BoundsMid);
		InsertStmt.SetBindingValueByIndex(9, ShadowEntry.Entry.BoundsMax);
		InsertStmt.SetBindingValueByIndex(10, ShadowEntry.Entry.Volume);
		InsertStmt.SetBindingValueByIndex(11, ShadowEntry.Entry.SizeClass);
		InsertStmt.SetBindingValueByIndex(12, ShadowEntry.Entry.Category);
		InsertStmt.SetBindingValueByIndex(13, static_cast<int64>(ShadowEntry.Entry.TriCount));
		InsertStmt.SetBindingValueByIndex(14, static_cast<int64>(ShadowEntry.Entry.bHasCollision ? 1 : 0));
		InsertStmt.SetBindingValueByIndex(15, static_cast<int64>(ShadowEntry.Entry.LodCount));
		InsertStmt.SetBindingValueByIndex(16, ShadowEntry.Entry.PivotOffsetZ);
		InsertStmt.SetBindingValueByIndex(17, static_cast<int64>(ShadowEntry.Entry.bDegenerate ? 1 : 0));
		InsertStmt.SetBindingValueByIndex(18, MonolithIndexDatabaseInternal::RowHashToHex(ShadowEntry.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}

	return true;
}

TArray<FMonolithShadowIndexedMeshCatalogEntry> FMonolithIndexDatabase::GetShadowMeshCatalogEntriesForAsset(const FString& CohortName, const int64 AssetId)
{
	return MonolithIndexDatabaseInternal::QueryActiveShadowRowsForAsset<FMonolithShadowIndexedMeshCatalogEntry>(
		Database,
		MakeShadowTableName(CohortName, TEXT("mesh_catalog")),
		AssetId,
		TEXT("s.asset_path, s.bounds_x, s.bounds_y, s.bounds_z, s.bounds_min, s.bounds_mid, s.bounds_max, s.volume, s.size_class, s.category, s.tri_count, s.has_collision, s.lod_count, s.pivot_offset_z, s.degenerate, s.row_hash"),
		TEXT("s.asset_path, s.row_hash"),
		[AssetId](FSQLitePreparedStatement& Stmt, FMonolithShadowIndexedMeshCatalogEntry& Row)
		{
			Row.Entry.AssetId = AssetId;
			Stmt.GetColumnValueByIndex(0, Row.Entry.AssetPath);
			Stmt.GetColumnValueByIndex(1, Row.Entry.BoundsX);
			Stmt.GetColumnValueByIndex(2, Row.Entry.BoundsY);
			Stmt.GetColumnValueByIndex(3, Row.Entry.BoundsZ);
			Stmt.GetColumnValueByIndex(4, Row.Entry.BoundsMin);
			Stmt.GetColumnValueByIndex(5, Row.Entry.BoundsMid);
			Stmt.GetColumnValueByIndex(6, Row.Entry.BoundsMax);
			Stmt.GetColumnValueByIndex(7, Row.Entry.Volume);
			Stmt.GetColumnValueByIndex(8, Row.Entry.SizeClass);
			Stmt.GetColumnValueByIndex(9, Row.Entry.Category);
			Stmt.GetColumnValueByIndex(10, Row.Entry.TriCount);
			int32 HasCollision = 0;
			int32 Degenerate = 0;
			Stmt.GetColumnValueByIndex(11, HasCollision);
			Stmt.GetColumnValueByIndex(12, Row.Entry.LodCount);
			Stmt.GetColumnValueByIndex(13, Row.Entry.PivotOffsetZ);
			Stmt.GetColumnValueByIndex(14, Degenerate);
			Row.Entry.bHasCollision = HasCollision != 0;
			Row.Entry.bDegenerate = Degenerate != 0;
			FString RowHashHex;
			Stmt.GetColumnValueByIndex(15, RowHashHex);
			Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		});
}

FMonolithShadowMeshCatalogAggregate FMonolithIndexDatabase::GetProductionMeshCatalogAggregateForAsset(const int64 AssetId)
{
	FMonolithShadowMeshCatalogAggregate Aggregate;
	const TOptional<FIndexedMeshCatalogEntry> Entry = GetMeshCatalogEntryForAsset(AssetId);
	if (Entry.IsSet())
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeMeshCatalogRowHash(Entry.GetValue());
	}

	return Aggregate;
}

FMonolithShadowMeshCatalogAggregate FMonolithIndexDatabase::GetShadowMeshCatalogAggregateForAsset(const FString& CohortName, const int64 AssetId)
{
	FMonolithShadowMeshCatalogAggregate Aggregate;
	for (const FMonolithShadowIndexedMeshCatalogEntry& Row : GetShadowMeshCatalogEntriesForAsset(CohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}

	return Aggregate;
}

bool FMonolithIndexDatabase::UpsertShadowTableRetention(
	const FString& CohortName,
	const FString& BaseTableName,
	const FDateTime& ExpiresAtUtc,
	const bool bRollbackRetained)
{
	if (!IsOpen())
	{
		return false;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		TEXT("INSERT INTO shadow_table_retention (table_name, cohort_name, base_table_name, expires_at_utc, rollback_retained) VALUES (?, ?, ?, ?, ?) ")
		TEXT("ON CONFLICT(table_name) DO UPDATE SET cohort_name = excluded.cohort_name, base_table_name = excluded.base_table_name, expires_at_utc = excluded.expires_at_utc, rollback_retained = excluded.rollback_retained;"));
	Stmt.SetBindingValueByIndex(1, MakeShadowTableName(CohortName, BaseTableName));
	Stmt.SetBindingValueByIndex(2, CohortName);
	Stmt.SetBindingValueByIndex(3, BaseTableName);
	Stmt.SetBindingValueByIndex(4, ExpiresAtUtc.ToIso8601());
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(bRollbackRetained ? 1 : 0));
	return Stmt.Execute();
}

TArray<FMonolithShadowTableRetention> FMonolithIndexDatabase::GetShadowTableRetentions()
{
	TArray<FMonolithShadowTableRetention> Result;
	if (!IsOpen())
	{
		return Result;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT cohort_name, base_table_name, table_name, expires_at_utc, rollback_retained FROM shadow_table_retention ORDER BY table_name;"));
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithShadowTableRetention Retention;
		FString ExpiresAtUtc;
		int64 RollbackRetained = 0;
		Stmt.GetColumnValueByIndex(0, Retention.CohortName);
		Stmt.GetColumnValueByIndex(1, Retention.BaseTableName);
		Stmt.GetColumnValueByIndex(2, Retention.TableName);
		Stmt.GetColumnValueByIndex(3, ExpiresAtUtc);
		Stmt.GetColumnValueByIndex(4, RollbackRetained);
		FDateTime::ParseIso8601(*ExpiresAtUtc, Retention.ExpiresAtUtc);
		Retention.bRollbackRetained = RollbackRetained != 0;
		Result.Add(MoveTemp(Retention));
	}

	return Result;
}

int32 FMonolithIndexDatabase::DropExpiredShadowTables(const FDateTime& NowUtc)
{
	if (!IsOpen())
	{
		return 0;
	}

	TArray<FString> ExpiredTableNames;
	FSQLitePreparedStatement SelectStmt;
	if (!SelectStmt.Create(*Database, TEXT("SELECT table_name FROM shadow_table_retention WHERE expires_at_utc <= ?;")))
	{
		UE_LOG(
			LogMonolithIndex,
			Error,
			TEXT("DropExpiredShadowTables: failed to prepare retention scan for %s: %s"),
			*DbPath,
			*Database->GetLastError());
		return 0;
	}
	SelectStmt.SetBindingValueByIndex(1, NowUtc.ToIso8601());
	while (SelectStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString TableName;
		SelectStmt.GetColumnValueByIndex(0, TableName);
		ExpiredTableNames.Add(MoveTemp(TableName));
	}

	if (ExpiredTableNames.Num() > 0)
	{
		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("DropExpiredShadowTables: found %d expired shadow table(s) in %s"),
			ExpiredTableNames.Num(),
			*DbPath);
	}

	int32 DroppedCount = 0;
	for (const FString& TableName : ExpiredTableNames)
	{
		UE_LOG(
			LogMonolithIndex,
			Log,
			TEXT("DropExpiredShadowTables: dropping expired shadow table %s from %s"),
			*TableName,
			*DbPath);
		ExecuteSQL(
			FString::Printf(TEXT("DROP TABLE IF EXISTS %s;"), *TableName),
			TEXT("DropExpiredShadowTables/DropShadowTable"));

		FSQLitePreparedStatement DeleteStmt;
		if (!DeleteStmt.Create(*Database, TEXT("DELETE FROM shadow_table_retention WHERE table_name = ?;")))
		{
			UE_LOG(
				LogMonolithIndex,
				Error,
				TEXT("DropExpiredShadowTables: failed to prepare retention delete for %s (table=%s): %s"),
				*DbPath,
				*TableName,
				*Database->GetLastError());
			continue;
		}
		DeleteStmt.SetBindingValueByIndex(1, TableName);
		if (DeleteStmt.Execute())
		{
			++DroppedCount;
		}
		else
		{
			UE_LOG(
				LogMonolithIndex,
				Error,
				TEXT("DropExpiredShadowTables: failed to delete retention row for %s (table=%s): %s"),
				*DbPath,
				*TableName,
				*Database->GetLastError());
		}
	}

	return DroppedCount;
}

FString FMonolithIndexDatabase::GetSavedHash(const FString& PackagePath)
{
	if (!IsOpen()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT saved_hash FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Hash;
		Stmt.GetColumnValueByIndex(0, Hash);
		return Hash;
	}
	return FString();
}

TMap<FString, FString> FMonolithIndexDatabase::GetAllPathsAndHashes()
{
	TMap<FString, FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path, saved_hash FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path, Hash;
		Stmt.GetColumnValueByIndex(0, Path);
		Stmt.GetColumnValueByIndex(1, Hash);
		Result.Add(MoveTemp(Path), MoveTemp(Hash));
	}
	return Result;
}

bool FMonolithIndexDatabase::DeleteAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return false;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return false;

	return DeleteAssetAndRelated(AssetId);
}

bool FMonolithIndexDatabase::UpdateAssetPath(const FString& OldPath, const FString& NewPath, const FString& NewAssetName)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET package_path = ?, asset_name = COALESCE(NULLIF(?, ''), asset_name) WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, NewPath);
	Stmt.SetBindingValueByIndex(2, NewAssetName);
	Stmt.SetBindingValueByIndex(3, OldPath);

	if (!Stmt.Execute()) return false;

	// Check if a row was actually updated
	// GetLastInsertRowId isn't useful for UPDATE; use changes count via a follow-up query
	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

bool FMonolithIndexDatabase::UpdateAssetMetadata(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET asset_name = ?, asset_class = ?, module_name = ?, description = ?, file_size_bytes = ?, last_modified = ?, saved_hash = ?, indexed_at = datetime('now') WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, Asset.AssetName);
	Stmt.SetBindingValueByIndex(2, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(3, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(4, Asset.Description);
	Stmt.SetBindingValueByIndex(5, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(6, Asset.LastModified);
	Stmt.SetBindingValueByIndex(7, Asset.SavedHash);
	Stmt.SetBindingValueByIndex(8, Asset.PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// Deletes per-asset child data that deep indexing repopulates.
bool FMonolithIndexDatabase::DeleteChildDataForAsset(int64 AssetId)
{
	if (!IsOpen()) return false;

	bool bSuccess = true;

	FSQLitePreparedStatement Stmt1;
	Stmt1.Create(*Database, TEXT("DELETE FROM nodes WHERE asset_id = ?;"));
	Stmt1.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt1.Execute();

	FSQLitePreparedStatement Stmt2;
	Stmt2.Create(*Database, TEXT("DELETE FROM variables WHERE asset_id = ?;"));
	Stmt2.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt2.Execute();

	FSQLitePreparedStatement Stmt3;
	Stmt3.Create(*Database, TEXT("DELETE FROM parameters WHERE asset_id = ?;"));
	Stmt3.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt3.Execute();

	FSQLitePreparedStatement Stmt4;
	Stmt4.Create(*Database, TEXT("DELETE FROM datatable_rows WHERE asset_id = ?;"));
	Stmt4.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt4.Execute();

	FSQLitePreparedStatement Stmt5;
	Stmt5.Create(*Database, TEXT("DELETE FROM actors WHERE asset_id = ?;"));
	Stmt5.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt5.Execute();

	FSQLitePreparedStatement Stmt6;
	Stmt6.Create(*Database, TEXT("DELETE FROM mesh_catalog WHERE asset_id = ?;"));
	Stmt6.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt6.Execute();

	FSQLitePreparedStatement Stmt7;
	Stmt7.Create(*Database, TEXT("DELETE FROM dependencies WHERE source_asset_id = ?;"));
	Stmt7.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt7.Execute();

	FSQLitePreparedStatement Stmt8;
	Stmt8.Create(*Database, TEXT("DELETE FROM tag_references WHERE asset_id = ?;"));
	Stmt8.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt8.Execute();

	return bSuccess;
}

bool FMonolithIndexDatabase::UpdateSavedHash(const FString& PackagePath, const FString& HashHex)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET saved_hash = ? WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, HashHex);
	Stmt.SetBindingValueByIndex(2, PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

int64 FMonolithIndexDatabase::ResolveActiveRevisionId(const int64 AssetId) const
{
	if (const int64* RevisionId = ActiveAssetRevisions.Find(AssetId))
	{
		return *RevisionId;
	}

	return 0;
}

int64 FMonolithIndexDatabase::ResolveWriteOrCurrentRevisionId(const int64 AssetId) const
{
	const int64 PendingRevisionId = ResolveActiveRevisionId(AssetId);
	if (PendingRevisionId > 0)
	{
		return PendingRevisionId;
	}

	if (!IsOpen() || AssetId <= 0)
	{
		return 0;
	}

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*Database, TEXT("SELECT current_revision_id FROM assets WHERE id = ?;")))
	{
		return 0;
	}

	Stmt.SetBindingValueByIndex(1, AssetId);
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 CurrentRevisionId = 0;
		Stmt.GetColumnValueByIndex(0, CurrentRevisionId);
		return CurrentRevisionId;
	}

	return 0;
}

bool FMonolithIndexDatabase::DeleteSupersededAssetRevisionRows(const int64 AssetId, const int64 KeepRevisionId)
{
	auto DeleteRowsByAssetColumn = [this, AssetId, KeepRevisionId](const TCHAR* TableName, const TCHAR* AssetColumnName) -> bool
	{
		FSQLitePreparedStatement DeleteStmt;
		const FString Sql = FString::Printf(TEXT("DELETE FROM %s WHERE %s = ? AND revision_id <> ?;"), TableName, AssetColumnName);
		DeleteStmt.Create(*Database, *Sql);
		DeleteStmt.SetBindingValueByIndex(1, AssetId);
		DeleteStmt.SetBindingValueByIndex(2, KeepRevisionId);
		return DeleteStmt.Execute();
	};

	return DeleteRowsByAssetColumn(TEXT("parameters"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("variables"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("nodes"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("actors"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("datatable_rows"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("mesh_catalog"), TEXT("asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("dependencies"), TEXT("source_asset_id"))
		&& DeleteRowsByAssetColumn(TEXT("tag_references"), TEXT("asset_id"))
		&& [&]() -> bool
		{
			auto DeleteShadowRowsMatching = [this, &DeleteRowsByAssetColumn](const TCHAR* Pattern) -> bool
			{
				FSQLitePreparedStatement ShadowTableStmt;
				if (!ShadowTableStmt.Create(*Database, TEXT("SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE ?;")))
				{
					return false;
				}

				ShadowTableStmt.SetBindingValueByIndex(1, Pattern);
				while (ShadowTableStmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString ShadowTableName;
					ShadowTableStmt.GetColumnValueByIndex(0, ShadowTableName);
					if (!DeleteRowsByAssetColumn(*ShadowTableName, TEXT("asset_id")))
					{
						return false;
					}
				}

				return true;
			};

			return DeleteShadowRowsMatching(TEXT("shadow_%_nodes"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_variables"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_actors"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_datatable_rows"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_mesh_catalog"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_parameters"))
				&& DeleteShadowRowsMatching(TEXT("shadow_%_connections"));
		}();
}

// ============================================================
// FTS5 Full-text search
// ============================================================

TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch(const FString& Query, int32 Limit)
{
	TArray<FSearchResult> Results;
	if (!IsOpen()) return Results;

	// Search assets FTS
	FString SQL = FString::Printf(
		TEXT("SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank FROM fts_assets f JOIN assets a ON a.id = f.rowid WHERE fts_assets MATCH ? ORDER BY rank LIMIT %d;"),
		Limit
	);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);
	Stmt.SetBindingValueByIndex(1, Query);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FSearchResult R;
		Stmt.GetColumnValueByIndex(0, R.AssetPath);
		Stmt.GetColumnValueByIndex(1, R.AssetName);
		Stmt.GetColumnValueByIndex(2, R.AssetClass);
		Stmt.GetColumnValueByIndex(3, R.ModuleName);
		Stmt.GetColumnValueByIndex(4, R.MatchContext);
		double RankD = 0.0;
		Stmt.GetColumnValueByIndex(5, RankD);
		R.Rank = static_cast<float>(RankD);
		Results.Add(MoveTemp(R));
	}

	// Also search nodes FTS
	FString NodeSQL = FString::Printf(
		TEXT("SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank FROM fts_nodes f JOIN nodes n ON n.id = f.rowid JOIN assets a ON a.id = n.asset_id WHERE fts_nodes MATCH ? AND (n.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND n.revision_id = 0)) ORDER BY f.rank LIMIT %d;"),
		Limit
	);

	FSQLitePreparedStatement Stmt2;
	Stmt2.Create(*Database, *NodeSQL);
	Stmt2.SetBindingValueByIndex(1, Query);

	while (Stmt2.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FSearchResult R;
		Stmt2.GetColumnValueByIndex(0, R.AssetPath);
		Stmt2.GetColumnValueByIndex(1, R.AssetName);
		Stmt2.GetColumnValueByIndex(2, R.AssetClass);
		Stmt2.GetColumnValueByIndex(3, R.ModuleName);
		Stmt2.GetColumnValueByIndex(4, R.MatchContext);
		double RankD = 0.0;
		Stmt2.GetColumnValueByIndex(5, RankD);
		R.Rank = static_cast<float>(RankD);
		Results.Add(MoveTemp(R));
	}

	// Sort combined results by rank (lower = better in FTS5)
	Results.Sort([](const FSearchResult& A, const FSearchResult& B) { return A.Rank < B.Rank; });

	if (Results.Num() > Limit)
	{
		Results.SetNum(Limit);
	}

	return Results;
}

// ============================================================
// Stats
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetStats()
{
	auto Stats = MakeShared<FJsonObject>();
	if (!IsOpen()) return Stats;

	auto GetCount = [this](const TCHAR* Table) -> int64
	{
		FSQLitePreparedStatement Stmt;
		FString SQL = FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table);
		Stmt.Create(*Database, *SQL);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return Count;
		}
		return 0;
	};

	Stats->SetNumberField(TEXT("assets"), GetCount(TEXT("assets")));
	Stats->SetNumberField(TEXT("nodes"), GetCount(TEXT("nodes")));
	Stats->SetNumberField(TEXT("connections"), GetCount(TEXT("connections")));
	Stats->SetNumberField(TEXT("variables"), GetCount(TEXT("variables")));
	Stats->SetNumberField(TEXT("parameters"), GetCount(TEXT("parameters")));
	Stats->SetNumberField(TEXT("dependencies"), GetCount(TEXT("dependencies")));
	Stats->SetNumberField(TEXT("actors"), GetCount(TEXT("actors")));
	Stats->SetNumberField(TEXT("tags"), GetCount(TEXT("tags")));
	Stats->SetNumberField(TEXT("configs"), GetCount(TEXT("configs")));
	Stats->SetNumberField(TEXT("cpp_symbols"), GetCount(TEXT("cpp_symbols")));
	Stats->SetNumberField(TEXT("datatable_rows"), GetCount(TEXT("datatable_rows")));
	Stats->SetNumberField(TEXT("mesh_catalog"), GetCount(TEXT("mesh_catalog")));

	// Asset class breakdown
	auto ClassBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20;"));
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ClassName;
		int64 Count = 0;
		Stmt.GetColumnValueByIndex(0, ClassName);
		Stmt.GetColumnValueByIndex(1, Count);
		ClassBreakdown->SetNumberField(ClassName, Count);
	}
	Stats->SetObjectField(TEXT("asset_class_breakdown"), ClassBreakdown);

	// Module breakdown (which plugins have how many assets)
	auto ModuleBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement ModStmt;
	ModStmt.Create(*Database, TEXT("SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC;"));
	while (ModStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ModName;
		int64 Count = 0;
		ModStmt.GetColumnValueByIndex(0, ModName);
		ModStmt.GetColumnValueByIndex(1, Count);
		ModuleBreakdown->SetNumberField(ModName, Count);
	}
	Stats->SetObjectField(TEXT("module_breakdown"), ModuleBreakdown);

	return Stats;
}

// ============================================================
// Asset details
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetAssetDetails(const FString& PackagePath)
{
	auto Details = MakeShared<FJsonObject>();
	if (!IsOpen()) return Details;

	auto MaybeAsset = GetAssetByPath(PackagePath);
	if (!MaybeAsset.IsSet()) return Details;

	const FIndexedAsset& Asset = MaybeAsset.GetValue();
	Details->SetStringField(TEXT("package_path"), Asset.PackagePath);
	Details->SetStringField(TEXT("asset_name"), Asset.AssetName);
	Details->SetStringField(TEXT("asset_class"), Asset.AssetClass);
	Details->SetStringField(TEXT("module_name"), Asset.ModuleName);
	Details->SetStringField(TEXT("description"), Asset.Description);
	Details->SetNumberField(TEXT("file_size_bytes"), Asset.FileSizeBytes);
	Details->SetStringField(TEXT("last_modified"), Asset.LastModified);
	Details->SetStringField(TEXT("indexed_at"), Asset.IndexedAt);
	Details->SetNumberField(TEXT("current_revision_id"), Asset.CurrentRevisionId);

	// Nodes
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (const auto& Node : GetNodesForAsset(Asset.Id))
	{
		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_type"), Node.NodeType);
		NodeObj->SetStringField(TEXT("node_name"), Node.NodeName);
		NodeObj->SetStringField(TEXT("node_class"), Node.NodeClass);

		// Include stored properties (type-specific metadata from indexers)
		if (!Node.Properties.IsEmpty() && Node.Properties != TEXT("{}"))
		{
			TSharedPtr<FJsonObject> PropsObj;
			auto Reader = TJsonReaderFactory<>::Create(Node.Properties);
			if (FJsonSerializer::Deserialize(Reader, PropsObj) && PropsObj.IsValid() && PropsObj->Values.Num() > 0)
			{
				NodeObj->SetObjectField(TEXT("properties"), PropsObj);
			}
		}

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Details->SetArrayField(TEXT("nodes"), NodesArr);

	// Variables
	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (const auto& Var : GetVariablesForAsset(Asset.Id))
	{
		auto VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName);
		VarObj->SetStringField(TEXT("type"), Var.VarType);
		VarObj->SetStringField(TEXT("category"), Var.Category);
		VarObj->SetBoolField(TEXT("exposed"), Var.bIsExposed);
		VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	Details->SetArrayField(TEXT("variables"), VarsArr);

	// DataTable rows
	TArray<TSharedPtr<FJsonValue>> DataTableRowsArr;
	for (const FIndexedDataTableRow& Row : GetDataTableRowsForAsset(Asset.Id))
	{
		auto RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), Row.RowName);

		if (!Row.RowData.IsEmpty() && Row.RowData != TEXT("{}"))
		{
			TSharedPtr<FJsonObject> RowDataObj;
			auto Reader = TJsonReaderFactory<>::Create(Row.RowData);
			if (FJsonSerializer::Deserialize(Reader, RowDataObj) && RowDataObj.IsValid() && RowDataObj->Values.Num() > 0)
			{
				RowObj->SetObjectField(TEXT("row_data"), RowDataObj);
			}
		}

		DataTableRowsArr.Add(MakeShared<FJsonValueObject>(RowObj));
	}
	Details->SetArrayField(TEXT("datatable_rows"), DataTableRowsArr);

	// Mesh catalog
	const TOptional<FIndexedMeshCatalogEntry> MeshCatalogEntry = GetMeshCatalogEntryForAsset(Asset.Id);
	if (MeshCatalogEntry.IsSet())
	{
		auto MeshCatalogObj = MakeShared<FJsonObject>();
		MeshCatalogObj->SetStringField(TEXT("asset_path"), MeshCatalogEntry->AssetPath);
		MeshCatalogObj->SetNumberField(TEXT("bounds_x"), MeshCatalogEntry->BoundsX);
		MeshCatalogObj->SetNumberField(TEXT("bounds_y"), MeshCatalogEntry->BoundsY);
		MeshCatalogObj->SetNumberField(TEXT("bounds_z"), MeshCatalogEntry->BoundsZ);
		MeshCatalogObj->SetNumberField(TEXT("bounds_min"), MeshCatalogEntry->BoundsMin);
		MeshCatalogObj->SetNumberField(TEXT("bounds_mid"), MeshCatalogEntry->BoundsMid);
		MeshCatalogObj->SetNumberField(TEXT("bounds_max"), MeshCatalogEntry->BoundsMax);
		MeshCatalogObj->SetNumberField(TEXT("volume"), MeshCatalogEntry->Volume);
		MeshCatalogObj->SetStringField(TEXT("size_class"), MeshCatalogEntry->SizeClass);
		MeshCatalogObj->SetStringField(TEXT("category"), MeshCatalogEntry->Category);
		MeshCatalogObj->SetNumberField(TEXT("tri_count"), MeshCatalogEntry->TriCount);
		MeshCatalogObj->SetBoolField(TEXT("has_collision"), MeshCatalogEntry->bHasCollision);
		MeshCatalogObj->SetNumberField(TEXT("lod_count"), MeshCatalogEntry->LodCount);
		MeshCatalogObj->SetNumberField(TEXT("pivot_offset_z"), MeshCatalogEntry->PivotOffsetZ);
		MeshCatalogObj->SetBoolField(TEXT("degenerate"), MeshCatalogEntry->bDegenerate);
		Details->SetObjectField(TEXT("mesh_catalog"), MeshCatalogObj);
	}

	// Dependencies — wrap in safety check to prevent invalid JSON propagation
	auto Refs = FindReferences(PackagePath);
	if (Refs.IsValid())
	{
		// Validate the references object can serialize cleanly
		FString SerializedRefs;
		auto Writer = TJsonWriterFactory<>::Create(&SerializedRefs);
		if (FJsonSerializer::Serialize(Refs.ToSharedRef(), Writer))
		{
			Details->SetObjectField(TEXT("references"), Refs);
		}
		else
		{
			// Fallback: provide empty references rather than invalid JSON
			auto EmptyRefs = MakeShared<FJsonObject>();
			EmptyRefs->SetArrayField(TEXT("depends_on"), TArray<TSharedPtr<FJsonValue>>());
			EmptyRefs->SetArrayField(TEXT("referenced_by"), TArray<TSharedPtr<FJsonValue>>());
			Details->SetObjectField(TEXT("references"), EmptyRefs);
		}
	}

	return Details;
}

// ============================================================
// Find by type
// ============================================================

TArray<FIndexedAsset> FMonolithIndexDatabase::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	TArray<FIndexedAsset> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at, current_revision_id FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?;"));
	Stmt.SetBindingValueByIndex(1, AssetClass);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Offset));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Stmt.GetColumnValueByIndex(10, Asset.CurrentRevisionId);
		Result.Add(MoveTemp(Asset));
	}
	return Result;
}

// ============================================================
// Find references (bidirectional dependency lookup)
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::FindReferences(const FString& PackagePath)
{
	auto Result = MakeShared<FJsonObject>();
	if (!IsOpen()) return Result;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return Result;

	// What this asset depends on
	TArray<TSharedPtr<FJsonValue>> DepsArr;
	for (const auto& Dep : GetDependenciesForAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Dep.TargetAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto DepObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
			DepObj->SetStringField(TEXT("path"), Path);
			DepObj->SetStringField(TEXT("class"), Class);
			DepObj->SetStringField(TEXT("type"), Dep.DependencyType);
			DepsArr.Add(MakeShared<FJsonValueObject>(DepObj));
		}
	}
	Result->SetArrayField(TEXT("depends_on"), DepsArr);

	// What references this asset
	TArray<TSharedPtr<FJsonValue>> RefsArr;
	for (const auto& Ref : GetReferencersOfAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Ref.SourceAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto RefObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
			RefObj->SetStringField(TEXT("path"), Path);
			RefObj->SetStringField(TEXT("class"), Class);
			RefObj->SetStringField(TEXT("type"), Ref.DependencyType);
			RefsArr.Add(MakeShared<FJsonValueObject>(RefObj));
		}
	}
	Result->SetArrayField(TEXT("referenced_by"), RefsArr);

	return Result;
}

// ============================================================
// Internal helpers
// ============================================================

bool FMonolithIndexDatabase::CreateTables()
{
	if (!Database || !Database->IsValid())
	{
		return false;
	}

	// GCreateTablesSQL contains multiple statements separated by semicolons.
	// FSQLiteDatabase::Execute() only handles one statement at a time,
	// so we split and execute each individually.
	FString FullSQL(GCreateTablesSQL);
	TArray<FString> Statements;

	// Split on semicolons, tracking BEGIN/END depth for trigger bodies
	int32 Start = 0;
	int32 Depth = 0;
	for (int32 i = 0; i < FullSQL.Len(); ++i)
	{
		// Check for BEGIN keyword (trigger body start)
		if (i + 5 <= FullSQL.Len())
		{
			FString Word = FullSQL.Mid(i, 5).ToUpper();
			if (Word == TEXT("BEGIN") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
			{
				if (i + 5 >= FullSQL.Len() || FChar::IsWhitespace(FullSQL[i + 5]) || FullSQL[i + 5] == '\n')
				{
					Depth++;
				}
			}
		}
		// Check for END keyword (trigger body end)
		if (i + 3 <= FullSQL.Len())
		{
			FString Word = FullSQL.Mid(i, 3).ToUpper();
			if (Word == TEXT("END") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
			{
				if (i + 3 >= FullSQL.Len() || FullSQL[i + 3] == ';' || FChar::IsWhitespace(FullSQL[i + 3]))
				{
					if (Depth > 0) Depth--;
				}
			}
		}

		if (FullSQL[i] == ';' && Depth == 0)
		{
			FString Stmt = FullSQL.Mid(Start, i - Start + 1).TrimStartAndEnd();
			if (!Stmt.IsEmpty() && Stmt != TEXT(";"))
			{
				Statements.Add(Stmt);
			}
			Start = i + 1;
		}
	}

	bool bAllSucceeded = true;
	for (const FString& Stmt : Statements)
	{
		if (!Database->Execute(*Stmt))
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Schema statement failed: %s -- Error: %s"),
				*Stmt.Left(100), *Database->GetLastError());
			bAllSucceeded = false;
			// Don't stop -- try remaining statements (some may be IF NOT EXISTS)
		}
	}

	if (!bAllSucceeded)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Some schema statements failed -- FTS5 may not be available in this SQLite build"));
	}

	return true; // Return true even if FTS fails -- basic tables should work
}

bool FMonolithIndexDatabase::ExecuteSQL(const FString& SQL, const TCHAR* Context)
{
	const TCHAR* SafeContext = Context ? Context : TEXT("General");
	const FString CompactSql = MonolithIndexDatabaseInternal::CompactSqlForLog(SQL);

	if (!Database || !Database->IsValid())
	{
		UE_LOG(
			LogMonolithIndex,
			Error,
			TEXT("Cannot execute SQL -- database not open (context=%s, db=%s, sql=%s)"),
			SafeContext,
			*DbPath,
			*CompactSql);
		return false;
	}

	if (!Database->Execute(*SQL))
	{
		UE_LOG(
			LogMonolithIndex,
			Error,
			TEXT("SQL execution failed (context=%s, db=%s, sql=%s): %s"),
			SafeContext,
			*DbPath,
			*CompactSql,
			*Database->GetLastError());
		return false;
	}
	return true;
}

// ============================================================
// AssetVisual CRUD（geometric / semantic 双 cohort 共用接口，按 CohortName 落到不同表）
// ============================================================

namespace AssetVisualDatabaseInternal
{
	/** 把 cohort 名映射到 production 表名；未识别的 cohort 返回空字符串。 */
	static FString GetProductionTableName(const FString& CohortName)
	{
		if (CohortName.Equals(TEXT("AssetVisualGeometric"), ESearchCase::IgnoreCase))
		{
			return TEXT("asset_visual_geometric");
		}
		if (CohortName.Equals(TEXT("AssetVisualSemantic"), ESearchCase::IgnoreCase))
		{
			return TEXT("asset_visual_semantic");
		}
		UE_LOG(LogMonolithIndex, Error,
			TEXT("AssetVisual production table 不识别 cohort 名：'%s'"), *CohortName);
		return FString();
	}
}

bool FMonolithIndexDatabase::ClearAssetVisualEntries(const FString& CohortName)
{
	if (!IsOpen())
	{
		return false;
	}
	const FString TableName = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (TableName.IsEmpty())
	{
		return false;
	}
	return ExecuteSQL(*FString::Printf(TEXT("DELETE FROM %s;"), *TableName));
}

int64 FMonolithIndexDatabase::InsertAssetVisualEntry(const FString& CohortName, const FIndexedAssetVisualEntry& Entry)
{
	if (!IsOpen())
	{
		return -1;
	}
	const FString TableName = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (TableName.IsEmpty())
	{
		return -1;
	}

	FSQLitePreparedStatement Stmt;
	const FString Sql = FString::Printf(
		TEXT("INSERT INTO %s (asset_id, revision_id, asset_path, shard_id, shard_prefix_depth, provider_id, provider_version, render_recipe_version, embedding_dim, embedding_dtype, embedding_bytes, preview_view_path) ")
		TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"),
		*TableName);
	Stmt.Create(*Database, *Sql);
	const int64 Revision = Entry.RevisionId > 0 ? Entry.RevisionId : ResolveActiveRevisionId(Entry.AssetId);
	Stmt.SetBindingValueByIndex(1, Entry.AssetId);
	Stmt.SetBindingValueByIndex(2, Revision);
	Stmt.SetBindingValueByIndex(3, Entry.AssetPath);
	Stmt.SetBindingValueByIndex(4, Entry.ShardId);
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Entry.ShardPrefixDepth));
	Stmt.SetBindingValueByIndex(6, Entry.ProviderId);
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Entry.ProviderVersion));
	Stmt.SetBindingValueByIndex(8, static_cast<int64>(Entry.RenderRecipeVersion));
	Stmt.SetBindingValueByIndex(9, static_cast<int64>(Entry.EmbeddingDim));
	Stmt.SetBindingValueByIndex(10, static_cast<int64>(Entry.EmbeddingDtype));
	// SQLite 的 SetBindingValueByIndex 重载 4 参数版本就是 BLOB 入口（指针 + 字节数 + bCopy）。
	Stmt.SetBindingValueByIndex(11, static_cast<const void*>(Entry.EmbeddingBytes.GetData()), static_cast<int32>(Entry.EmbeddingBytes.Num()), false);
	Stmt.SetBindingValueByIndex(12, Entry.PreviewViewPath);

	if (!Stmt.Execute())
	{
		return -1;
	}
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedAssetVisualEntry> FMonolithIndexDatabase::GetAssetVisualEntryForAsset(const FString& CohortName, const int64 AssetId)
{
	if (!IsOpen())
	{
		return {};
	}
	const FString TableName = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (TableName.IsEmpty())
	{
		return {};
	}

	const FString Sql = FString::Printf(
		TEXT("SELECT m.id, m.asset_id, m.revision_id, m.asset_path, m.shard_id, m.shard_prefix_depth, m.provider_id, m.provider_version, m.render_recipe_version, m.embedding_dim, m.embedding_dtype, m.embedding_bytes, m.preview_view_path ")
		TEXT("FROM %s m JOIN assets a ON a.id = m.asset_id ")
		TEXT("WHERE m.asset_id = ? AND (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0));"),
		*TableName);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *Sql);
	Stmt.SetBindingValueByIndex(1, AssetId);

	if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return {};
	}

	FIndexedAssetVisualEntry Entry;
	Stmt.GetColumnValueByIndex(0, Entry.Id);
	Stmt.GetColumnValueByIndex(1, Entry.AssetId);
	Stmt.GetColumnValueByIndex(2, Entry.RevisionId);
	Stmt.GetColumnValueByIndex(3, Entry.AssetPath);
	Stmt.GetColumnValueByIndex(4, Entry.ShardId);
	int64 PrefixDepth = 0;
	Stmt.GetColumnValueByIndex(5, PrefixDepth);
	Entry.ShardPrefixDepth = static_cast<int32>(PrefixDepth);
	Stmt.GetColumnValueByIndex(6, Entry.ProviderId);
	int64 ProviderVersion = 1;
	int64 RenderVersion = 1;
	Stmt.GetColumnValueByIndex(7, ProviderVersion);
	Stmt.GetColumnValueByIndex(8, RenderVersion);
	Entry.ProviderVersion = static_cast<uint32>(ProviderVersion);
	Entry.RenderRecipeVersion = static_cast<uint32>(RenderVersion);
	int64 EmbeddingDim = 0;
	int64 EmbeddingDtype = 0;
	Stmt.GetColumnValueByIndex(9, EmbeddingDim);
	Stmt.GetColumnValueByIndex(10, EmbeddingDtype);
	Entry.EmbeddingDim = static_cast<int32>(EmbeddingDim);
	Entry.EmbeddingDtype = static_cast<uint8>(EmbeddingDtype);
	Stmt.GetColumnValueByIndex(11, Entry.EmbeddingBytes);
	Stmt.GetColumnValueByIndex(12, Entry.PreviewViewPath);
	return Entry;
}

TArray<FIndexedAssetVisualEntry> FMonolithIndexDatabase::GetAssetVisualEntries(const FString& CohortName, const FString& ShardIdFilter)
{
	TArray<FIndexedAssetVisualEntry> Result;
	if (!IsOpen())
	{
		return Result;
	}
	const FString TableName = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (TableName.IsEmpty())
	{
		return Result;
	}

	FString Sql = FString::Printf(
		TEXT("SELECT m.id, m.asset_id, m.revision_id, m.asset_path, m.shard_id, m.shard_prefix_depth, m.provider_id, m.provider_version, m.render_recipe_version, m.embedding_dim, m.embedding_dtype, m.embedding_bytes, m.preview_view_path ")
		TEXT("FROM %s m JOIN assets a ON a.id = m.asset_id ")
		TEXT("WHERE (m.revision_id = a.current_revision_id OR (a.current_revision_id = 0 AND m.revision_id = 0))"),
		*TableName);
	if (!ShardIdFilter.IsEmpty())
	{
		Sql.Append(TEXT(" AND m.shard_id = ?"));
	}
	Sql.Append(TEXT(";"));

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *Sql);
	if (!ShardIdFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(1, ShardIdFilter);
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAssetVisualEntry Entry;
		Stmt.GetColumnValueByIndex(0, Entry.Id);
		Stmt.GetColumnValueByIndex(1, Entry.AssetId);
		Stmt.GetColumnValueByIndex(2, Entry.RevisionId);
		Stmt.GetColumnValueByIndex(3, Entry.AssetPath);
		Stmt.GetColumnValueByIndex(4, Entry.ShardId);
		int64 PrefixDepth = 0;
		Stmt.GetColumnValueByIndex(5, PrefixDepth);
		Entry.ShardPrefixDepth = static_cast<int32>(PrefixDepth);
		Stmt.GetColumnValueByIndex(6, Entry.ProviderId);
		int64 ProviderVersion = 1;
		int64 RenderVersion = 1;
		Stmt.GetColumnValueByIndex(7, ProviderVersion);
		Stmt.GetColumnValueByIndex(8, RenderVersion);
		Entry.ProviderVersion = static_cast<uint32>(ProviderVersion);
		Entry.RenderRecipeVersion = static_cast<uint32>(RenderVersion);
		int64 EmbeddingDim = 0;
		int64 EmbeddingDtype = 0;
		Stmt.GetColumnValueByIndex(9, EmbeddingDim);
		Stmt.GetColumnValueByIndex(10, EmbeddingDtype);
		Entry.EmbeddingDim = static_cast<int32>(EmbeddingDim);
		Entry.EmbeddingDtype = static_cast<uint8>(EmbeddingDtype);
		Stmt.GetColumnValueByIndex(11, Entry.EmbeddingBytes);
		Stmt.GetColumnValueByIndex(12, Entry.PreviewViewPath);
		Result.Add(MoveTemp(Entry));
	}
	return Result;
}

bool FMonolithIndexDatabase::ReplaceShadowAssetVisualEntriesForAsset(
	const FString& CohortName,
	const FString& ShadowCohortName,
	const int64 AssetId,
	const TArray<FMonolithShadowIndexedAssetVisualEntry>& Entries)
{
	if (!IsOpen() || AssetId <= 0)
	{
		return false;
	}
	const FString BaseTable = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (BaseTable.IsEmpty())
	{
		return false;
	}
	// shadow 表名通过 shadow cohort 名来 namespace，避免不同实验串用同一份 shadow 数据。
	const FString TableName = MakeShadowTableName(ShadowCohortName, BaseTable);

	// 第一次写入时按需建表；schema 与生产表对齐 + 加 row_hash 列。
	const FString CreateSql = FString::Printf(
		TEXT("CREATE TABLE IF NOT EXISTS %s ("
			"    id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"    asset_id INTEGER NOT NULL,"
			"    revision_id INTEGER DEFAULT 0,"
			"    asset_path TEXT NOT NULL,"
			"    shard_id TEXT NOT NULL DEFAULT '',"
			"    shard_prefix_depth INTEGER DEFAULT 0,"
			"    provider_id TEXT NOT NULL DEFAULT '',"
			"    provider_version INTEGER DEFAULT 1,"
			"    render_recipe_version INTEGER DEFAULT 1,"
			"    embedding_dim INTEGER DEFAULT 0,"
			"    embedding_dtype INTEGER DEFAULT 0,"
			"    embedding_bytes BLOB,"
			"    preview_view_path TEXT DEFAULT '',"
			"    row_hash TEXT DEFAULT ''"
			");"),
		*TableName);
	ExecuteSQL(*CreateSql);

	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);

	// 先按 (asset_id, revision_id) 清掉旧 shadow 行；保证一次替换是一致的整体快照。
	FSQLitePreparedStatement DeleteStmt;
	DeleteStmt.Create(*Database, *FString::Printf(TEXT("DELETE FROM %s WHERE asset_id = ? AND revision_id = ?;"), *TableName));
	DeleteStmt.SetBindingValueByIndex(1, AssetId);
	DeleteStmt.SetBindingValueByIndex(2, ActiveRevisionId);
	if (!DeleteStmt.Execute())
	{
		return false;
	}

	for (const FMonolithShadowIndexedAssetVisualEntry& ShadowEntry : Entries)
	{
		const int64 RevisionId = ShadowEntry.Entry.RevisionId > 0 ? ShadowEntry.Entry.RevisionId : ActiveRevisionId;
		FSQLitePreparedStatement InsertStmt;
		InsertStmt.Create(
			*Database,
			*FString::Printf(
				TEXT("INSERT INTO %s (asset_id, revision_id, asset_path, shard_id, shard_prefix_depth, provider_id, provider_version, render_recipe_version, embedding_dim, embedding_dtype, embedding_bytes, preview_view_path, row_hash) ")
				TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"),
				*TableName));
		InsertStmt.SetBindingValueByIndex(1, AssetId);
		InsertStmt.SetBindingValueByIndex(2, RevisionId);
		InsertStmt.SetBindingValueByIndex(3, ShadowEntry.Entry.AssetPath);
		InsertStmt.SetBindingValueByIndex(4, ShadowEntry.Entry.ShardId);
		InsertStmt.SetBindingValueByIndex(5, static_cast<int64>(ShadowEntry.Entry.ShardPrefixDepth));
		InsertStmt.SetBindingValueByIndex(6, ShadowEntry.Entry.ProviderId);
		InsertStmt.SetBindingValueByIndex(7, static_cast<int64>(ShadowEntry.Entry.ProviderVersion));
		InsertStmt.SetBindingValueByIndex(8, static_cast<int64>(ShadowEntry.Entry.RenderRecipeVersion));
		InsertStmt.SetBindingValueByIndex(9, static_cast<int64>(ShadowEntry.Entry.EmbeddingDim));
		InsertStmt.SetBindingValueByIndex(10, static_cast<int64>(ShadowEntry.Entry.EmbeddingDtype));
		InsertStmt.SetBindingValueByIndex(11, static_cast<const void*>(ShadowEntry.Entry.EmbeddingBytes.GetData()), static_cast<int32>(ShadowEntry.Entry.EmbeddingBytes.Num()), false);
		InsertStmt.SetBindingValueByIndex(12, ShadowEntry.Entry.PreviewViewPath);
		InsertStmt.SetBindingValueByIndex(13, MonolithIndexDatabaseInternal::RowHashToHex(ShadowEntry.RowHash));
		if (!InsertStmt.Execute())
		{
			return false;
		}
	}
	return true;
}

TArray<FMonolithShadowIndexedAssetVisualEntry> FMonolithIndexDatabase::GetShadowAssetVisualEntriesForAsset(
	const FString& CohortName,
	const FString& ShadowCohortName,
	const int64 AssetId)
{
	TArray<FMonolithShadowIndexedAssetVisualEntry> Result;
	if (!IsOpen())
	{
		return Result;
	}
	const FString BaseTable = AssetVisualDatabaseInternal::GetProductionTableName(CohortName);
	if (BaseTable.IsEmpty())
	{
		return Result;
	}
	const FString TableName = MakeShadowTableName(ShadowCohortName, BaseTable);

	const int64 ActiveRevisionId = ResolveActiveRevisionId(AssetId);
	FSQLitePreparedStatement Stmt;
	Stmt.Create(
		*Database,
		*FString::Printf(
			TEXT("SELECT s.asset_path, s.shard_id, s.shard_prefix_depth, s.provider_id, s.provider_version, s.render_recipe_version, s.embedding_dim, s.embedding_dtype, s.embedding_bytes, s.preview_view_path, s.row_hash ")
			TEXT("FROM %s s WHERE s.asset_id = ? AND s.revision_id = ?;"),
			*TableName));
	Stmt.SetBindingValueByIndex(1, AssetId);
	Stmt.SetBindingValueByIndex(2, ActiveRevisionId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithShadowIndexedAssetVisualEntry Row;
		Row.Entry.AssetId = AssetId;
		Row.Entry.RevisionId = ActiveRevisionId;
		Stmt.GetColumnValueByIndex(0, Row.Entry.AssetPath);
		Stmt.GetColumnValueByIndex(1, Row.Entry.ShardId);
		int64 PrefixDepth = 0;
		Stmt.GetColumnValueByIndex(2, PrefixDepth);
		Row.Entry.ShardPrefixDepth = static_cast<int32>(PrefixDepth);
		Stmt.GetColumnValueByIndex(3, Row.Entry.ProviderId);
		int64 ProviderVersion = 1;
		int64 RenderVersion = 1;
		Stmt.GetColumnValueByIndex(4, ProviderVersion);
		Stmt.GetColumnValueByIndex(5, RenderVersion);
		Row.Entry.ProviderVersion = static_cast<uint32>(ProviderVersion);
		Row.Entry.RenderRecipeVersion = static_cast<uint32>(RenderVersion);
		int64 EmbeddingDim = 0;
		int64 EmbeddingDtype = 0;
		Stmt.GetColumnValueByIndex(6, EmbeddingDim);
		Stmt.GetColumnValueByIndex(7, EmbeddingDtype);
		Row.Entry.EmbeddingDim = static_cast<int32>(EmbeddingDim);
		Row.Entry.EmbeddingDtype = static_cast<uint8>(EmbeddingDtype);
		Stmt.GetColumnValueByIndex(8, Row.Entry.EmbeddingBytes);
		Stmt.GetColumnValueByIndex(9, Row.Entry.PreviewViewPath);
		FString RowHashHex;
		Stmt.GetColumnValueByIndex(10, RowHashHex);
		Row.RowHash = MonolithIndexDatabaseInternal::ParseRowHashHex(RowHashHex);
		Result.Add(MoveTemp(Row));
	}
	return Result;
}

FMonolithShadowAssetVisualAggregate FMonolithIndexDatabase::GetProductionAssetVisualAggregateForAsset(
	const FString& CohortName,
	const int64 AssetId)
{
	FMonolithShadowAssetVisualAggregate Aggregate;
	const TOptional<FIndexedAssetVisualEntry> Entry = GetAssetVisualEntryForAsset(CohortName, AssetId);
	if (Entry.IsSet())
	{
		extern uint64 ComputeAssetVisualRowHash(const FIndexedAssetVisualEntry&);
		++Aggregate.RowCount;
		Aggregate.RowHashSum += ComputeAssetVisualRowHash(Entry.GetValue());
	}
	return Aggregate;
}

FMonolithShadowAssetVisualAggregate FMonolithIndexDatabase::GetShadowAssetVisualAggregateForAsset(
	const FString& CohortName,
	const FString& ShadowCohortName,
	const int64 AssetId)
{
	FMonolithShadowAssetVisualAggregate Aggregate;
	for (const FMonolithShadowIndexedAssetVisualEntry& Row : GetShadowAssetVisualEntriesForAsset(CohortName, ShadowCohortName, AssetId))
	{
		++Aggregate.RowCount;
		Aggregate.RowHashSum += Row.RowHash;
	}
	return Aggregate;
}
