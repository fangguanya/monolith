---
name: unreal-python
description: Use when executing raw UE Editor Python over Monolith MCP — SCS/SubobjectDataSubsystem inspection, CDO manipulation, EditorAssetLibrary bulk ops, anything not yet wrapped by a domain namespace. Triggers on unreal.*, EditorAssetLibrary, SubobjectDataSubsystem, get_editor_property, set_editor_property, exec in editor, python console.
---

# Unreal Python via Monolith MCP

3 actions on `python_query` that wrap `IPythonScriptPlugin::ExecPythonCommandEx`. Available only when `UMonolithSettings::bEnablePython=true` (off by default — RCE surface).

## Actions

| Action | Purpose |
|--------|---------|
| `execute` | Multi-statement Python source (`import`, `def`, loops OK). Uses `ExecuteFile` mode. |
| `execute_file` | Read a `.py` from absolute disk path and run it. |
| `evaluate` | Single expression, returns repr in `command_result`. Statements error. |

## Common params

| Param | Where | Notes |
|-------|-------|-------|
| `command` | `execute` | Required. Python source string. |
| `path` | `execute_file` | Required. Absolute path to a `.py`. |
| `expression` | `evaluate` | Required. Single Python expression. |
| `scope` | `execute`, `execute_file` | `"private"` (default, isolated globals) or `"public"` (shared with editor Python console — stateful across calls). |
| `unattended` | `execute`, `execute_file` | Default `true`. Suppress dialogs. |

## Response shape

```
{
  success, command_result, elapsed_ms,
  stdout, stderr, warnings,           // joined per type
  log_output: [{ type, text }, ...],  // granular
  log_count
}
```

`unreal.log` → `stdout`. `unreal.log_warning` → `warnings`. `unreal.log_error` and Python tracebacks → `stderr`.

## When to use

**Reach for `python_query` when a domain namespace doesn't already cover what you need.** Domain namespaces (`blueprint_query`, `mesh_query`, etc.) are faster and return structured JSON. `python_query` is the escape hatch for SCS/Subobject APIs, niche `EditorAssetLibrary` calls, and for batching several `unreal.*` calls into one round-trip.

## Enable

`Saved/Config/WindowsEditor/Monolith.ini`:

```
[/Script/MonolithCore.MonolithSettings]
bEnablePython=True
```

Or Project Settings → Monolith → Modules|Optional → **Enable Python Execution Module (DANGEROUS)**. Then restart the editor and verify:

```
LogMonolith: Monolith — Python module loaded (3 actions)
```

## Gotcha

`set_editor_property` on a **native-inherited** SkeletalMeshComponent (e.g. the inherited `Mesh` of `AWheeledVehiclePawn`) mutates the CDO in memory but `EditorAssetLibrary.save_asset` may silently drop the change. Verify the saved .uasset if you're editing an inherited component; prefer adding a new SCS component for persistent edits.
