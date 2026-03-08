# Changelog

All notable changes to Monolith will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.5.0] - 2026-03-08

Auto-updater rewrite — fixes all swap script failures on Windows.

### Fixed

- **Auto-Updater** — Swap script now polls `tasklist` for `UnrealEditor.exe` instead of a cosmetic 10-second countdown (was launching before editor fully exited)
- **Auto-Updater** — `errorlevel` check after retry rename was unreachable due to cmd.exe resetting `%ERRORLEVEL%` on closing `)` — replaced with `goto` pattern
- **Auto-Updater** — Launcher script now uses outer-double-quote trick for `cmd /c` paths with spaces (`D:\Unreal Projects\...`)
- **Auto-Updater** — Switched from `ren` (bare filename only) to `move` (full path support) for plugin folder rename
- **Auto-Updater** — Retry now cleans stale backup before re-attempting rename
- **Auto-Updater** — Rollback on failed xcopy now removes partial destination before restoring backup
- **Auto-Updater** — Added `/h` flag to primary xcopy to include hidden-attribute files
- **Auto-Updater** — Enabled `DelayedExpansion` for correct variable expansion inside `if` blocks

## [0.2.0] - 2026-03-08

Source indexer overhaul and auto-updater improvements.

### Fixed

- **Source Indexer** — UE macros (UCLASS, ENGINE_API, GENERATED_BODY) now stripped before tree-sitter parsing, fixing class hierarchy and inheritance resolution
- **Source Indexer** — Class definitions increased from ~0 to 62,059; inheritance links from ~0 to 37,010
- **Source Indexer** — `read_source members_only` now returns class members correctly
- **Source Indexer** — `get_class_hierarchy` ancestor traversal now works
- **MonolithSource** — `get_class_hierarchy` accepts both `symbol` and `class_name` params (was inconsistent)

### Added

- **Source Indexer** — UE macro preprocessor (`ue_preprocessor.py`) with balanced-paren stripping for UCLASS/USTRUCT/UENUM/UINTERFACE
- **Source Indexer** — `--clean` flag for `__main__.py` to delete DB before reindexing
- **Source Indexer** — Diagnostic output after indexing (definitions, forward decls, inheritance stats)
- **Auto-Updater** — Release notes now shown in update notification toast and logged to Output Log

### Changed

- **Source Indexer** — `reference_builder.py` now preprocesses source before tree-sitter parsing

### Important

- **You MUST delete your existing source database and reindex** after updating to 0.2.0. The old database has empty class hierarchy data. Delete the `.db` file in your Saved/Monolith/ directory and run the indexer with `--clean`.

## [0.1.0] - 2026-03-07

Initial beta release. One plugin, 9 domains, 119 actions.

### Added

- **MonolithCore** — Embedded Streamable HTTP MCP server with JSON-RPC 2.0 dispatch
- **MonolithCore** — Central tool registry with discovery/dispatch pattern (~14 namespace tools instead of ~117 individual tools)
- **MonolithCore** — Plugin settings via UDeveloperSettings (port, auto-update, module toggles, DB paths)
- **MonolithCore** — Auto-updater via GitHub Releases (download, stage, notify)
- **MonolithCore** — Asset loading with 4-tier fallback (StaticLoadObject -> PackageName.ObjectName -> FindObject+_C -> ForEachObjectWithPackage)
- **MonolithBlueprint** — 5 actions: graph topology, variables, execution flow tracing, node search
- **MonolithMaterial** — 14 actions: inspection, graph editing, build/export/import, validation, preview rendering, Custom HLSL nodes
- **MonolithAnimation** — 23 actions: montage sections, blend space samples, ABP graph reading, notify editing, bone tracks, skeleton info
- **MonolithNiagara** — 39 actions: system/emitter management, module stack operations, parameters, renderers, batch execute, declarative system builder
- **MonolithNiagara** — 6 reimplemented NiagaraEditor helpers (Epic APIs not exported)
- **MonolithEditor** — 13 actions: Live Coding build triggers, compile output capture, log ring buffer (10K entries), crash context
- **MonolithConfig** — 6 actions: INI resolution, explain (multi-layer), diff from default, search, section read
- **MonolithIndex** — SQLite FTS5 project indexer with 4 indexers (Blueprint, Material, Generic, Dependency)
- **MonolithIndex** — 5 actions: full-text search, reference tracing, type filtering, stats, asset deep inspection
- **MonolithSource** — Python tree-sitter engine source indexer (C++ and shader parsing)
- **MonolithSource** — 10 actions: source reading, call graphs, class hierarchy, symbol context, module info
- **9 Claude Code skills** — Domain-specific workflow guides for animation, blueprints, build decisions, C++, debugging, materials, Niagara, performance, project search
- **Templates** — `.mcp.json.example` and `CLAUDE.md.example` for quick project setup
- All 9 modules compiling clean on UE 5.7
- **MonolithEditor** — `get_compile_output` action for Live Coding compile result capture with time-windowed error filtering
- **MonolithEditor** — Auto hot-swap on editor exit (stages update, swaps on close)
- **MonolithEditor** — Re-index buttons in Project Settings UI
- **MonolithEditor** — Improved Live Coding integration with compile output capture, time-windowed errors, category filtering
- **unreal-build skill** — Smart build decision-making guide (Live Coding vs full rebuild)
- **Logging** — 80% reduction in Log-level noise across all modules (kept Warnings/Errors, demoted routine logs to Verbose)
- **README** — Complete rewrite with Installation for Dummies walkthrough

### Fixed

- HTTP body null-termination causing malformed JSON-RPC responses
- Niagara graph traversal crash when accessing emitter shared graphs
- Niagara emitter lookup failures — added case-insensitive matching with fallbacks
- Source DB WAL journal mode causing lock contention — switched to DELETE mode
- SQL schema creation with nested BEGIN/END depth tracking for triggers
- Reindex dispatch — switched from `FindFunctionByName` to `StartFullIndex` with UFUNCTION
- Asset loading crash from `FastGetAsset` on background thread — removed unsafe call
- Animation `remove_bone_track` — now uses `RemoveBoneCurve(FName)` per bone with child traversal
- MonolithIndex `last_full_index` — added `WriteMeta()` call, guarded with `!bShouldStop`
- Niagara `move_module` — rewires stack-flow pins only, preserves override inputs
- Editor `get_build_errors` — uses `ELogVerbosity` enum instead of substring matching
- MonolithIndex SQL injection — all 13 insert methods converted to `FSQLitePreparedStatement`
- Animation modules using `LogTemp` instead of `LogMonolith`
- Editor `CachedLogCapture` dangling pointer — added `ClearCachedLogCapture()` in `ShutdownModule`
- MonolithSource vestigial outer module — flattened structure, deleted stub
- Session expiry / reconnection issues — removed session tracking entirely (server is stateless)
- Claude tools failing on first invocation — fixed transport type in `.mcp.json` (`"http"` -> `"streamableHttp"`) and fixed MonolithSource stub not registering actions
