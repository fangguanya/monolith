# Monolith — Blueprint Tooling Expansion Testing Checklist

Last updated: 2026-03-25

---

## Test Environment

- **Engine:** Unreal Engine 5.7
- **Project:** Leviathan (`D:\Unreal Projects\Leviathan\`)
- **Plugin location:** `Plugins/Monolith/`
- **MCP endpoint:** `http://localhost:9316/mcp`
- **Implementation plan:** `Docs/plans/2026-03-25-blueprint-tooling-expansion.md`
- **Test assets:** Use `/Game/Test/BPExpansion/` prefix for all test assets

---

## How to Test

Use the standard MCP test template from `TESTING.md`:

```python
import http.client, json

def test_action(namespace, action, params=None):
    conn = http.client.HTTPConnection("localhost", 9316)
    if namespace == "monolith":
        tool_name = f"monolith_{action}"
        args = params or {}
    else:
        tool_name = f"{namespace}_query"
        args = {"action": action}
        if params:
            args["params"] = params

    body = json.dumps({
        "jsonrpc": "2.0", "id": 1,
        "method": "tools/call",
        "params": {"name": tool_name, "arguments": args}
    })
    conn.request("POST", "/mcp", body, {"Content-Type": "application/json"})
    resp = conn.getresponse()
    result = json.loads(resp.read())
    conn.close()
    return result
```

### Pre-test Setup

Before running tests, create the base test Blueprints:

```python
# Create test Actor Blueprint
test_action("blueprint", "create_blueprint", {
    "asset_path": "/Game/Test/BPExpansion/BP_TestActor",
    "parent_class": "Actor"
})

# Create a second BP for comparison/copy tests
test_action("blueprint", "create_blueprint", {
    "asset_path": "/Game/Test/BPExpansion/BP_TestActorB",
    "parent_class": "Actor"
})

# Add a function graph for Return node and export tests
test_action("blueprint", "add_function", {
    "asset_path": "/Game/Test/BPExpansion/BP_TestActor",
    "function_name": "TestFunc"
})
```

---

## Current Status: 0/105 TESTS PASS (all PENDING)

---

## Phase 1 — Quick Wins

### 1A. save_asset

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.01 | `save_asset` | Save a dirty Blueprint to disk | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor"}` | Success, returns asset path and dirty=false | |
| 1.02 | `save_asset` | Save a non-Blueprint UObject (DataAsset) | `{"asset_path": "/Game/Test/BPExpansion/DA_TestData"}` | Success, works on any loaded asset (pre-create a DataAsset first) | |
| 1.03 | `save_asset` | Save with invalid path | `{"asset_path": "/Game/Test/BPExpansion/DoesNotExist"}` | Error: asset not found | |

### 1B. remove_macro / rename_macro

Pre-step: Create a macro in BP_TestActor:

```python
test_action("blueprint", "add_macro", {
    "asset_path": "/Game/Test/BPExpansion/BP_TestActor",
    "macro_name": "TestMacro"
})
```

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.04 | `rename_macro` | Rename existing macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "macro_name": "TestMacro", "new_name": "RenamedMacro"}` | Success, macro renamed. Verify via `get_functions` or discover. | |
| 1.05 | `rename_macro` | Rename non-existent macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "macro_name": "FakeMacro", "new_name": "Whatever"}` | Error: macro not found | |
| 1.06 | `remove_macro` | Remove existing macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "macro_name": "RenamedMacro"}` | Success, macro removed from BP->MacroGraphs | |
| 1.07 | `remove_macro` | Remove non-existent macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "macro_name": "FakeMacro"}` | Error: macro not found | |

### 1C. add_node — Quick Win Aliases (Macro Instances)

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.08 | `add_node` | ForEachLoop macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "ForEachLoop"}` | Creates MacroInstance node for ForEachLoop from StandardMacros. Verify class via `get_graph_data`. | |
| 1.09 | `add_node` | ForLoop macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "ForLoop"}` | Creates MacroInstance for ForLoop | |
| 1.10 | `add_node` | ForLoopWithBreak macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "ForLoopWithBreak"}` | Creates MacroInstance for ForLoopWithBreak | |
| 1.11 | `add_node` | DoOnce macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "DoOnce"}` | Creates MacroInstance for DoOnce (NOT a K2Node — engine macro) | |
| 1.12 | `add_node` | FlipFlop macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "FlipFlop"}` | Creates MacroInstance for FlipFlop (NOT a K2Node — engine macro) | |
| 1.13 | `add_node` | Gate macro | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Gate"}` | Creates MacroInstance for Gate (NOT a K2Node — engine macro) | |

### 1D. add_node — Quick Win Aliases (Call Functions)

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.14 | `add_node` | IsValid function call | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "IsValid"}` | Creates CallFunction node for KismetSystemLibrary::IsValid | |
| 1.15 | `add_node` | Delay function call | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Delay"}` | Creates latent Delay node with Duration pin | |
| 1.16 | `add_node` | RetriggerableDelay function call | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "RetriggerableDelay"}` | Creates latent RetriggerableDelay node | |

### 1E. add_node — Trivial K2Node Handlers

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.17 | `add_node` | Self node | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Self"}` | Creates UK2Node_Self with output pin of BP's class type | |
| 1.18 | `add_node` | Return node in function graph | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Return", "graph_name": "TestFunc"}` | Creates UK2Node_FunctionResult in function graph | |
| 1.19 | `add_node` | Return node in event graph (reject) | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Return"}` | Error: Return nodes only allowed in function graphs | |

### 1F. add_node — Generic K2Node Fallback

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.20 | `add_node` | Generic fallback with valid K2Node class | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "InputAction"}` | Creates UK2Node_InputAction via fallback. Response includes warning about generic fallback. | |
| 1.21 | `add_node` | Generic fallback with invalid class | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "CompletelyFakeNodeType"}` | Error: unknown node type, lists available types | |

### 1G. Pin Name Case-Insensitive Matching

Pre-step: Add a Branch node and an event node to BP_TestActor for connection testing.

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.22 | `connect_pins` | Connect using wrong-case pin names | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "source_node": "<event_node_id>", "source_pin": "then", "target_node": "<branch_node_id>", "target_pin": "execute"}` | Success: case-insensitive match resolves "then" to "Then" and "execute" to "Execute" | |
| 1.23 | `connect_pins` | Connect with completely wrong pin name | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "source_node": "<branch_node_id>", "source_pin": "nonexistent_pin", "target_node": "<event_node_id>", "target_pin": "Execute"}` | Error: pin not found, message lists available pins (e.g., "Available pins: Execute, Condition, True, False") | |

### 1H. Compile Error Node ID Linking

Pre-step: Create a BP with broken connections (e.g., a node with an unconnected required pin).

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.24 | `compile_blueprint` | Compile BP with errors, check node_id in output | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor"}` | Compile errors include `node_id` and `graph_name` fields pointing to the offending node | |

### 1I. Shared Alias Map (resolve_node parity)

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 1.25 | `resolve_node` | Resolve ForEachLoop (macro alias) | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "ForEachLoop"}` | Returns pin information for ForEachLoop. Same alias recognized as in add_node. | |
| 1.26 | `resolve_node` | Resolve Self (trivial handler) | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Self"}` | Returns pin information for Self node | |

---

## Phase 2 — CDO + Tags

### 2A. add_node — Medium Handlers

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 2.01 | `add_node` | MakeStruct with Vector | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "MakeStruct", "struct_type": "Vector"}` | Creates Make Vector node with X, Y, Z input pins | |
| 2.02 | `add_node` | BreakStruct with Vector | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "BreakStruct", "struct_type": "Vector"}` | Creates Break Vector node with X, Y, Z output pins | |
| 2.03 | `add_node` | MakeStruct with invalid struct | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "MakeStruct", "struct_type": "NonExistentStruct"}` | Error: struct type not found, lists what was tried (with/without F prefix) | |
| 2.04 | `add_node` | SwitchOnEnum with engine enum | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "SwitchOnEnum", "enum_type": "ECollisionChannel"}` | Creates switch node with enum case pins for each ECollisionChannel value | |
| 2.05 | `add_node` | SwitchOnInt | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "SwitchOnInt"}` | Creates UK2Node_SwitchInteger with default pins | |
| 2.06 | `add_node` | SwitchOnString | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "SwitchOnString"}` | Creates UK2Node_SwitchString with default pins | |
| 2.07 | `add_node` | FormatText with format string | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "FormatText", "format": "Hello {Name}, you have {Count} items"}` | Creates FormatText node. Dynamic argument pins "Name" and "Count" auto-generated from format pattern. | |
| 2.08 | `add_node` | FormatText without format string | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "FormatText"}` | Creates FormatText node with default format pin only | |
| 2.09 | `add_node` | MakeArray with num_entries | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "MakeArray", "num_entries": 3}` | Creates MakeArray node with 3 input element pins (indices 0, 1, 2) | |
| 2.10 | `add_node` | Select node | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "Select"}` | Creates UK2Node_Select with default pins | |

### 2B. set_cdo_property

Pre-step: Add variables to BP_TestActor for CDO testing:

```python
test_action("blueprint", "add_variable", {"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "variable_name": "TestFloat", "type": "float"})
test_action("blueprint", "add_variable", {"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "variable_name": "TestBool", "type": "bool"})
test_action("blueprint", "add_variable", {"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "variable_name": "TestString", "type": "string"})
test_action("blueprint", "compile_blueprint", {"asset_path": "/Game/Test/BPExpansion/BP_TestActor"})
```

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 2.11 | `set_cdo_property` | Set float on Blueprint CDO | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "property_name": "TestFloat", "value": 42.5}` | Success. Verify via `get_cdo_properties` shows TestFloat=42.5 | |
| 2.12 | `set_cdo_property` | Set bool on Blueprint CDO | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "property_name": "TestBool", "value": true}` | Success. Verify via `get_cdo_properties` shows TestBool=true | |
| 2.13 | `set_cdo_property` | Set string on Blueprint CDO | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "property_name": "TestString", "value": "Hello World"}` | Success. Verify via `get_cdo_properties` shows TestString="Hello World" | |
| 2.14 | `set_cdo_property` | Case-insensitive property name | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "property_name": "testfloat", "value": 99.0}` | Success via case-insensitive fallback | |
| 2.15 | `set_cdo_property` | Non-existent property | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "property_name": "FakeProperty", "value": 0}` | Error: property not found | |
| 2.16 | `set_cdo_property` | Roundtrip: set then read | Set TestFloat=123.0, then `get_cdo_properties` | Readback confirms TestFloat=123.0 | |

### 2C. Gameplay Tag Queries

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 2.17 | `list_gameplay_tags` | List all tags (no filter) | `{}` | Returns all registered gameplay tags with names, descriptions, hierarchy | |
| 2.18 | `list_gameplay_tags` | Filter by prefix | `{"prefix": "Weapon"}` | Returns only tags under Weapon.* hierarchy | |
| 2.19 | `list_gameplay_tags` | Non-existent prefix | `{"prefix": "ZZZ.NonExistent"}` | Returns empty array, no error | |
| 2.20 | `search_gameplay_tags` | Search by substring | `{"query": "damage"}` | Returns matching tags + which assets reference them | |
| 2.21 | `search_gameplay_tags` | Search with no matches | `{"query": "zzzznonexistent"}` | Returns empty array, no error | |

---

## Phase 3 — Structural

### 3A. Timeline Operations

Pre-step: Create a timeline in BP_TestActor:

```python
test_action("blueprint", "add_timeline", {
    "asset_path": "/Game/Test/BPExpansion/BP_TestActor",
    "timeline_name": "TestTimeline"
})
```

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 3.01 | `get_timeline_data` | Read timeline data | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor"}` | Returns TestTimeline with name, GUID, auto_play, loop, length, empty tracks | |
| 3.02 | `get_timeline_data` | BP with no timelines | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActorB"}` | Returns empty timelines array, no error | |
| 3.03 | `add_timeline_track` | Add float track | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "AlphaTrack", "track_type": "float"}` | Success: float track added to TestTimeline | |
| 3.04 | `add_timeline_track` | Add vector track | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "PositionTrack", "track_type": "vector"}` | Success: vector track added | |
| 3.05 | `add_timeline_track` | Add event track | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "HitEvent", "track_type": "event"}` | Success: event track added | |
| 3.06 | `add_timeline_track` | Add color track | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "ColorFade", "track_type": "color"}` | Success: color track added | |
| 3.07 | `add_timeline_track` | Non-existent timeline | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "FakeTimeline", "track_name": "X", "track_type": "float"}` | Error: timeline not found | |
| 3.08 | `set_timeline_keys` | Set keyframes on float track | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "AlphaTrack", "keys": [{"time": 0.0, "value": 0.0}, {"time": 0.5, "value": 1.0}, {"time": 1.0, "value": 0.0}]}` | Success: 3 keys set. FRichCurve populated. | |
| 3.09 | `set_timeline_keys` | Set keys with interp_mode | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "timeline_name": "TestTimeline", "track_name": "AlphaTrack", "keys": [{"time": 0.0, "value": 0.0, "interp_mode": "cubic"}, {"time": 1.0, "value": 1.0, "interp_mode": "linear"}]}` | Success: keys with correct interpolation modes | |
| 3.10 | `get_timeline_data` | Roundtrip: read back tracks and keys | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor"}` | Returns all 4 tracks. AlphaTrack shows keys at t=0.0, t=1.0 with correct values and interp modes. | |

### 3B. User-Defined Struct and Enum

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 3.11 | `create_user_defined_struct` | Create struct with typed fields | `{"save_path": "/Game/Test/BPExpansion/S_TestStruct", "fields": [{"name": "Name", "type": "string"}, {"name": "Value", "type": "float"}, {"name": "IsActive", "type": "bool"}]}` | Success: struct created with 3 fields, visible in Content Browser | |
| 3.12 | `create_user_defined_struct` | Create struct with default values | `{"save_path": "/Game/Test/BPExpansion/S_TestDefaults", "fields": [{"name": "Health", "type": "float", "default_value": "100.0"}, {"name": "Label", "type": "string", "default_value": "Default"}]}` | Success: fields have specified defaults | |
| 3.13 | `create_user_defined_struct` | Duplicate path | `{"save_path": "/Game/Test/BPExpansion/S_TestStruct", "fields": [{"name": "X", "type": "float"}]}` | Error: asset already exists at path | |
| 3.14 | `create_user_defined_enum` | Create enum with values | `{"save_path": "/Game/Test/BPExpansion/E_TestEnum", "values": ["None", "Low", "Medium", "High", "Critical"]}` | Success: enum created with 5 values | |
| 3.15 | `create_user_defined_enum` | Verify enum usable in SwitchOnEnum | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "SwitchOnEnum", "enum_type": "E_TestEnum"}` via `add_node` | Creates switch node with None/Low/Medium/High/Critical case pins | |

### 3C. DataTable Operations

Pre-step: Ensure S_TestStruct exists from test 3.11.

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 3.16 | `create_data_table` | Create DataTable with row struct | `{"save_path": "/Game/Test/BPExpansion/DT_TestTable", "row_struct": "S_TestStruct"}` | Success: empty DataTable created with S_TestStruct row struct | |
| 3.17 | `create_data_table` | Invalid row struct | `{"save_path": "/Game/Test/BPExpansion/DT_Bad", "row_struct": "NonExistentStruct"}` | Error: struct not found | |
| 3.18 | `add_data_table_row` | Add first row | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable", "row_name": "Row_001", "values": {"Name": "Sword", "Value": 25.0, "IsActive": true}}` | Success: row added with all fields | |
| 3.19 | `add_data_table_row` | Add second row | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable", "row_name": "Row_002", "values": {"Name": "Shield", "Value": 50.0, "IsActive": false}}` | Success: second row added | |
| 3.20 | `add_data_table_row` | Duplicate row name | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable", "row_name": "Row_001", "values": {"Name": "Axe", "Value": 10.0, "IsActive": true}}` | Error or overwrite (document actual behavior) | |
| 3.21 | `get_data_table_rows` | Read all rows | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable"}` | Returns Row_001 and Row_002 with correct field values | |
| 3.22 | `get_data_table_rows` | Read single row | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable", "row_name": "Row_001"}` | Returns only Row_001: Name=Sword, Value=25.0, IsActive=true | |
| 3.23 | `get_data_table_rows` | Non-existent row | `{"asset_path": "/Game/Test/BPExpansion/DT_TestTable", "row_name": "Row_999"}` | Error or empty result: row not found | |

---

## Phase 4 — Build from Spec

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 4.01 | `build_blueprint_from_spec` | Full spec: variables + nodes + connections + pin defaults | See full spec below | BP created with Health variable, 3 nodes (CustomEvent, Branch, PrintString), 2 exec connections, pin default on InString. Compiles cleanly. | |
| 4.02 | `build_blueprint_from_spec` | Spec with components | `{"asset_path": "/Game/Test/BPExpansion/BP_SpecComp", "components": [{"name": "DamageBox", "class": "BoxComponent"}, {"name": "HitSphere", "class": "SphereComponent"}]}` | BP created with 2 components attached to root | |
| 4.03 | `build_blueprint_from_spec` | auto_compile=true | Add `"auto_compile": true` to 4.01 spec | BP compiles automatically, response includes compile status | |
| 4.04 | `build_blueprint_from_spec` | Invalid node in spec (partial failure) | Spec with one valid and one invalid node: `{"id": "bad", "type": "CompletelyFakeNode"}` | Partial success: valid nodes created, error for invalid node, other steps proceed | |
| 4.05 | `build_blueprint_from_spec` | Roundtrip: build then read | Build spec from 4.01, then `get_graph_data` | Graph data matches: correct nodes, connections, pin defaults | |
| 4.06 | `build_blueprint_from_spec` | Minimal spec (just asset_path) | `{"asset_path": "/Game/Test/BPExpansion/BP_SpecEmpty"}` | Creates empty BP successfully, no errors | |

**Full spec for test 4.01:**

```json
{
  "asset_path": "/Game/Test/BPExpansion/BP_SpecFull",
  "graph_name": "EventGraph",
  "variables": [
    {"name": "Health", "type": "float", "default": "100.0"}
  ],
  "nodes": [
    {"id": "evt", "type": "CustomEvent", "event_name": "OnDamage", "position": [0, 0]},
    {"id": "branch", "type": "Branch", "position": [300, 0]},
    {"id": "print", "type": "CallFunction", "function_name": "PrintString", "position": [600, 0]}
  ],
  "connections": [
    {"source": "evt", "source_pin": "Then", "target": "branch", "target_pin": "Execute"},
    {"source": "branch", "source_pin": "True", "target": "print", "target_pin": "Execute"}
  ],
  "pin_defaults": [
    {"node_id": "print", "pin_name": "InString", "value": "Took damage!"}
  ],
  "auto_compile": true
}
```

---

## Phase 5 — Export / RPC / Level

### 5A. RPC Parameters on CustomEvent and add_function

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 5.01 | `add_node` | CustomEvent with server RPC | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "CustomEvent", "event_name": "ServerRPC", "replication": "server", "reliable": true}` | Creates event with FUNC_Net + FUNC_NetServer + FUNC_NetReliable flags. Verify via `get_graph_data`. | |
| 5.02 | `add_node` | CustomEvent with client RPC | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "CustomEvent", "event_name": "ClientRPC", "replication": "client", "reliable": false}` | Creates event with FUNC_Net + FUNC_NetClient (no reliable) | |
| 5.03 | `add_node` | CustomEvent with multicast | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "CustomEvent", "event_name": "MulticastRPC", "replication": "multicast", "reliable": true}` | Creates event with FUNC_Net + FUNC_NetMulticast + FUNC_NetReliable | |
| 5.04 | `add_node` | CustomEvent with no replication (default) | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "node_type": "CustomEvent", "event_name": "LocalEvent"}` | No net flags set (baseline behavior unchanged) | |
| 5.05 | `add_function` | Function with server RPC | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "function_name": "ServerFunc", "replication": "server", "reliable": true}` | Function created with net flags. Verify via `get_functions`. | |

### 5B. Level Blueprint Loading

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 5.06 | `get_variables` | Load level BP by map path | `{"asset_path": "/Game/Maps/TestLevel"}` (use actual project map) | Returns level blueprint's variables via package load fallback | |
| 5.07 | `get_variables` | Load current level BP via $current sentinel | `{"asset_path": "$current"}` | Returns currently loaded level's blueprint variables | |
| 5.08 | `add_node` | Add node to level blueprint | `{"asset_path": "$current", "node_type": "PrintString"}` | Node added to current level's blueprint event graph | |

### 5C. Graph Export / Copy / Duplicate

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 5.09 | `export_graph` | Export event graph to JSON | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "graph_name": "EventGraph"}` | Returns JSON with nodes, connections, pin_defaults arrays. Schema matches `build_blueprint_from_spec` input. | |
| 5.10 | `export_graph` | Export function graph | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "graph_name": "TestFunc"}` | Returns JSON for function graph content | |
| 5.11 | `copy_nodes` | Copy nodes between BPs via T3D | `{"source_asset": "/Game/Test/BPExpansion/BP_TestActor", "source_graph": "EventGraph", "node_ids": ["<node_id_1>", "<node_id_2>"], "target_asset": "/Game/Test/BPExpansion/BP_TestActorB", "target_graph": "EventGraph"}` | Nodes copied. Internal connections preserved. New node IDs returned in response. | |
| 5.12 | `copy_nodes` | External connections silently dropped | Copy a node that has connections outside the copy set | Internal connections preserved. External connections dropped without error. | |
| 5.13 | `duplicate_graph` | Duplicate function graph | `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "graph_name": "TestFunc", "new_name": "TestFunc_Copy"}` | New function "TestFunc_Copy" created with same nodes and connections | |
| 5.14 | `duplicate_graph` | Duplicate macro graph | Pre-step: create a macro "TestMacro2". `{"asset_path": "/Game/Test/BPExpansion/BP_TestActor", "graph_name": "TestMacro2", "new_name": "TestMacro2_Copy"}` | New macro graph created as copy | |

---

## Phase 6 — Layout

Pre-step: Create a test BP with several connected nodes all at position (0,0):

```python
test_action("blueprint", "build_blueprint_from_spec", {
    "asset_path": "/Game/Test/BPExpansion/BP_LayoutTest",
    "nodes": [
        {"id": "begin", "type": "BeginPlay", "position": [0, 0]},
        {"id": "delay", "type": "Delay", "position": [0, 0]},
        {"id": "print1", "type": "CallFunction", "function_name": "PrintString", "position": [0, 0]},
        {"id": "branch", "type": "Branch", "position": [0, 0]},
        {"id": "print2", "type": "CallFunction", "function_name": "PrintString", "position": [0, 0]}
    ],
    "connections": [
        {"source": "begin", "source_pin": "Then", "target": "delay", "target_pin": "Execute"},
        {"source": "delay", "source_pin": "Completed", "target": "branch", "target_pin": "Execute"},
        {"source": "branch", "source_pin": "True", "target": "print1", "target_pin": "Execute"},
        {"source": "branch", "source_pin": "False", "target": "print2", "target_pin": "Execute"}
    ]
})
```

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 6.01 | `auto_layout` | Layout all nodes | `{"asset_path": "/Game/Test/BPExpansion/BP_LayoutTest", "graph_name": "EventGraph", "layout_mode": "all"}` | All nodes repositioned. Response includes positions_changed count. | |
| 6.02 | `auto_layout` | Verify no overlapping nodes | After 6.01, read positions via `get_graph_data` | No two nodes occupy overlapping bounding rectangles | |
| 6.03 | `auto_layout` | Verify exec flow left-to-right | After 6.01, check X positions | BeginPlay.X < Delay.X < Branch.X. Print nodes at or right of Branch. | |
| 6.04 | `auto_layout` | new_only mode (preserve existing) | Pre-step: move BeginPlay to (100,100), leave others at (0,0). `{"asset_path": "/Game/Test/BPExpansion/BP_LayoutTest", "layout_mode": "new_only"}` | BeginPlay stays at (100,100). Zero-position nodes get repositioned around it. | |
| 6.05 | `auto_layout` | selected mode (specific nodes) | `{"asset_path": "/Game/Test/BPExpansion/BP_LayoutTest", "layout_mode": "selected", "node_ids": ["<print1_id>", "<print2_id>"]}` | Only print1 and print2 repositioned. Others remain fixed as layout anchors. | |
| 6.06 | `auto_layout` | Comment box resizing | Pre-step: add a comment node around some nodes. Run `auto_layout` with `layout_mode: "all"`. | Comment box resized to encompass its child nodes + padding after layout | |
| 6.07 | `auto_layout` | Custom spacing parameters | `{"asset_path": "/Game/Test/BPExpansion/BP_LayoutTest", "layout_mode": "all", "horizontal_spacing": 500, "vertical_spacing": 120}` | Nodes visibly more spread out than default (350h, 80v) | |

---

## Phase 7 — Compare + Templates

### 7A. compare_blueprints

Pre-step: Ensure BP_TestActor and BP_TestActorB have different content (variables, components, nodes) by this point in testing. If needed, add distinct variables to each.

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 7.01 | `compare_blueprints` | Diff two different BPs | `{"asset_path_a": "/Game/Test/BPExpansion/BP_TestActor", "asset_path_b": "/Game/Test/BPExpansion/BP_TestActorB"}` | Returns structured diff: variables (added/removed/modified), components, functions, graphs. Summary with total_diffs. | |
| 7.02 | `compare_blueprints` | Diff BP with itself (no changes) | `{"asset_path_a": "/Game/Test/BPExpansion/BP_TestActorB", "asset_path_b": "/Game/Test/BPExpansion/BP_TestActorB"}` | Empty diff, total_diffs = 0 | |
| 7.03 | `compare_blueprints` | Diff with non-existent BP | `{"asset_path_a": "/Game/Test/BPExpansion/BP_TestActor", "asset_path_b": "/Game/Test/BPExpansion/DoesNotExist"}` | Error: asset not found | |

### 7B. apply_template

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 7.04 | `apply_template` | health_system template | `{"template_name": "health_system", "asset_path": "/Game/Test/BPExpansion/BP_HealthTest", "params": {"max_health": 200}}` | BP created with Health/MaxHealth variables, TakeDamage/Die/Heal functions. Compiles clean. | |
| 7.05 | `apply_template` | timer_loop template | `{"template_name": "timer_loop", "asset_path": "/Game/Test/BPExpansion/BP_TimerTest", "params": {"interval": 2.0, "event_name": "OnTick"}}` | BP created with SetTimerByEvent -> custom event -> retrigger pattern | |
| 7.06 | `apply_template` | interactable_actor template | `{"template_name": "interactable_actor", "asset_path": "/Game/Test/BPExpansion/BP_InteractTest", "params": {"interaction_text": "Press E to interact"}}` | BP created with interface impl, overlap, interaction prompt | |
| 7.07 | `apply_template` | Non-existent template | `{"template_name": "fake_template", "asset_path": "/Game/Test/BPExpansion/BP_FakeTest"}` | Error: template not found. Lists available templates. | |

### 7C. list_templates

| # | Action | Test | Params | Expected Result | Status |
|---|--------|------|--------|-----------------|--------|
| 7.08 | `list_templates` | List all available templates | `{}` | Returns array with at least: health_system, timer_loop, interactable_actor. Each has name and description. | |

---

## Test Summary

| Phase | Tests | Description | Status |
|-------|-------|-------------|--------|
| 1 | 1.01 -- 1.26 (26 tests) | save_asset, macro CRUD, add_node aliases (macro + function + trivial + fallback), pin case-insensitivity, compile node IDs, shared alias map | PENDING |
| 2 | 2.01 -- 2.21 (21 tests) | Medium add_node handlers (struct/enum/switch/format/array/select), set_cdo_property, gameplay tag queries | PENDING |
| 3 | 3.01 -- 3.23 (23 tests) | Timeline read/write/keys, user-defined struct/enum creation, DataTable CRUD | PENDING |
| 4 | 4.01 -- 4.06 (6 tests) | build_blueprint_from_spec (full spec, components, auto-compile, partial failure, roundtrip, minimal) | PENDING |
| 5 | 5.01 -- 5.14 (14 tests) | RPC params on events/functions, level blueprint loading ($current + map path), export_graph, copy_nodes, duplicate_graph | PENDING |
| 6 | 6.01 -- 6.07 (7 tests) | auto_layout modes (all, new_only, selected), overlap check, flow direction, comment resize, custom spacing | PENDING |
| 7 | 7.01 -- 7.08 (8 tests) | compare_blueprints, apply_template (3 templates), list_templates | PENDING |
| **Total** | **105 tests** | | **0/105 PASS** |

---

## Bugs Found

_To be filled during testing._

| # | Phase | Action | Issue | Fix | Verified |
|---|-------|--------|-------|-----|----------|
| | | | | | |

---

## Test History

| Date | Scope | Result | Notes |
|------|-------|--------|-------|
| | | | |
