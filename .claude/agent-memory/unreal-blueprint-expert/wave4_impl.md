---
name: wave4_impl
description: Wave 4 Bulk Node Operations implementation notes — add_nodes_bulk, connect_pins_bulk, set_pin_defaults_bulk
type: project
---

Wave 4 adds 3 actions (52 -> 55 total).

**Why:** Blueprint 47→67 plan, Wave 4 = Bulk Node Operations. Syntactic sugar over batch_execute with specialized return formats and temp_id support for add_nodes_bulk.

**How to apply:** Reference when extending or debugging these actions.

## Actions added

- `add_nodes_bulk` — NodeActions.h/.cpp. Transaction-wrapped loop over HandleAddNode. Handles EJson::Array and EJson::String (Claude Code quirk) for `nodes` param. Supports `auto_layout` (5-column grid, 200px horizontal, 100px vertical, applied only when entry lacks `position`). Returns `{nodes_created: [{temp_id, node_id, class, title}], count}`. Invalid entries (non-object JSON) are silently skipped.
- `connect_pins_bulk` — NodeActions.h/.cpp. Transaction-wrapped loop over HandleConnectPins. Handles JSON string quirk on `connections` param. Returns `{connected, failed, results: [{index, success, error?}]}`.
- `set_pin_defaults_bulk` — NodeActions.h/.cpp. Transaction-wrapped loop over HandleSetPinDefault. Handles JSON string quirk on `defaults` param. Returns `{set, failed, results: [{index, success, error?}]}`.

## Key gotchas

- `SerializeNode` uses field name `"id"` NOT `"node_id"`. When extracting from HandleAddNode result, read `Result.Result->GetStringField("id")` and expose it as `"node_id"` in the bulk response. This is the field callers use in connect_pins source_node/target_node.
- All three bulk handlers inject `asset_path` and `graph_name` (if present on parent params) into each sub-params before copying the per-entry fields. Per-entry fields override shared ones if they overlap (entry's own graph_name wins).
- `add_nodes_bulk` skips silently on invalid JSON entries (no temp_id available to report). `connect_pins_bulk` and `set_pin_defaults_bulk` report by index so they always include the entry in results.
- Transaction is a single `BeginTransaction`/`EndTransaction` wrapping the entire loop — not one per entry.

## Files modified

- `Source/MonolithBlueprint/Public/MonolithBlueprintNodeActions.h` — 3 new handler declarations
- `Source/MonolithBlueprint/Private/MonolithBlueprintNodeActions.cpp` — 3 new registrations + implementations
