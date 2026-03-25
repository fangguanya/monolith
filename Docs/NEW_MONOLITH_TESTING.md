# Monolith — Pending Test List

Last updated: 2026-03-25

Everything here needs editor-up MCP testing before the next release.

---

## Material Function Suite (9 new actions + 1 extended)

All implemented 2026-03-25. Zero tests run yet.

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 1 | `export_function_graph` | Load a MF with switches, inputs, outputs, custom HLSL, nested function calls. Verify: inputs array has InputName/InputType/PreviewValue, outputs have OutputName, nodes have props with parameter_name/group/sort_priority, switch params have default_value/dynamic_branch, connections have from/to/pin linkage, custom_hlsl_nodes separate. Test with `include_properties: false` and `include_positions: false`. | PENDING |
| 2 | `export_function_graph` (round-trip) | Take output JSON, feed inputs/outputs/nodes/custom_hlsl_nodes/connections into `build_function_graph` on a new function. Compare expression count and connection count. Known gap: `preview_value` numeric defaults don't round-trip. | PENDING |
| 3 | `set_function_metadata` | Change description, expose_to_library, library_categories on a test function. Verify via `get_function_info`. Check Material Function Library browser reflects changes without restart. | PENDING |
| 4 | `delete_function_expression` | Delete single expression, verify gone. Delete comma-separated batch, verify all gone + not_found reported for bad names. Verify MFI path is rejected with clear error. Verify undo works (Ctrl+Z in editor). | PENDING |
| 5 | `update_material_function` | Modify a function's graph, call update, verify referencing materials recompile. Test with both UMaterialFunction and UMaterialFunctionInstance paths. | PENDING |
| 6 | `create_function_instance` | Create MFI from a parent function with scalar, vector, texture, and static switch overrides. Verify overrides applied via `get_function_instance_info`. Test MaterialLayer and MaterialLayerBlend parent types create correct instance subclass. Test bad param names go to errors array, not hard fail. | PENDING |
| 7 | `set_function_instance_parameter` | Set each param type (scalar, vector, texture, switch) individually. Verify values via `get_function_instance_info`. Verify switch sets bOverride=true. Verify bad param name returns error. Verify recompile cascades. | PENDING |
| 8 | `get_function_instance_info` | Read a MFI with mixed overrides. Verify all 11 param type arrays present (scalar, vector, double_vector, texture, texture_collection, parameter_collection, font, rvt, svt, static_switch, static_component_mask). Verify parent/base chain. Verify inputs/outputs from GetInputsAndOutputs. | PENDING |
| 9 | `layout_function_expressions` | Call on a messy function, verify positions changed. Verify MFI path rejected. | PENDING |
| 10 | `rename_function_parameter_group` | Rename a group, verify via export_function_graph that parameter group field changed. | PENDING |
| 11 | `create_material_function` (type param) | Create MaterialLayer, verify type. Create MaterialLayerBlend, verify type. Pass bad type string, verify error. | PENDING |

---

## CyborgYL PR #8 Contributions (3 extended actions + 3 new Niagara)

Merged from PR #8, needs verification in our build.

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 12 | `get_all_expressions` (MF support) | Pass a MaterialFunction path. Verify expressions returned with `asset_type: "MaterialFunction"`. | PENDING |
| 13 | `get_expression_details` (MF support) | Pass a MaterialFunction path + expression name. Verify details returned. | PENDING |
| 14 | `set_expression_property` (MF support) | Set a property on an expression inside a MaterialFunction. Verify PreEditChange/PostEditChange fires correctly. | PENDING |
| 15 | `get_emitter_variables` | Niagara — verify returns emitter-scope variables. | PENDING |
| 16 | `get_system_variables` | Niagara — verify returns system-scope variables. | PENDING |
| 17 | `get_particle_attributes` | Niagara — verify returns particle attributes. | PENDING |

---

## Editor Fixes

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 18 | `delete_assets` (configurable prefixes) | Delete without `allowed_prefixes` — should work on any /Game/ path. Delete with `allowed_prefixes: ["/Game/Test/"]` — verify paths outside prefix are rejected. Verify `success` is false on partial delete. Verify `found` count in response. | PENDING |

---

## Code Review Fixes (from /simplify)

| # | Area | Test Plan | Status |
|---|------|-----------|--------|
| 19 | `delete_function_expression` undo | Delete an expression, Ctrl+Z in editor, verify it comes back. | PENDING |
| 20 | `export_function_graph` class names | Verify node class names have "MaterialExpression" prefix stripped (e.g. "Multiply" not "MaterialExpressionMultiply"). | PENDING |

---

## Niagara Phase 3-6B (21 new actions)

From previous sessions, committed but untested.

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 21 | `list_dynamic_inputs` | List DIs on a module. Verify returns array with names. | PENDING |
| 22 | `get_dynamic_input_tree` | Get DI tree for a module input. Verify nested structure. | PENDING |
| 23 | `remove_dynamic_input` | Remove a DI from a module input. Verify it's gone. | PENDING |
| 24 | `get_dynamic_input_value` | Read a DI value. | PENDING |
| 25 | `get_dynamic_input_inputs` | Read DI sub-inputs. | PENDING |
| 26 | `rename_emitter` | Rename an emitter. Verify new name persists. | PENDING |
| 27 | `get_emitter_property` | Read an emitter property by name. | PENDING |
| 28 | `list_available_renderers` | List renderer classes. Verify known types present (Sprite, Mesh, Ribbon). | PENDING |
| 29 | `set_renderer_mesh` | Set mesh on a MeshRenderer. Verify mesh path. | PENDING |
| 30 | `configure_ribbon` | Set ribbon properties. Verify via get. | PENDING |
| 31 | `configure_sub_uv` | Set SubUV animation params. Verify module added. | PENDING |
| 32 | `create_npc` | Create NiagaraParameterCollection. Verify asset exists. | PENDING |
| 33 | `get_npc` | Read NPC contents. | PENDING |
| 34 | `add_npc_parameter` | Add param to NPC. Verify via get. | PENDING |
| 35 | `remove_npc_parameter` | Remove param. Verify gone. | PENDING |
| 36 | `set_npc_default` | Set default value. Verify via get. | PENDING |
| 37 | `create_effect_type` | Create NiagaraEffectType. Verify asset. | PENDING |
| 38 | `get_effect_type` | Read effect type properties. | PENDING |
| 39 | `set_effect_type_property` | Set property on effect type. | PENDING |
| 40 | `get_available_parameters` | List available params for a system/emitter. | PENDING |
| 41 | `preview_system` | Capture a Niagara system viewport frame. Verify image saved. | PENDING |

---

## Animation Expansion Wave 1: Asset Creation (9 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 42 | `create_blend_space` | Create BS with skeleton, verify asset exists. Test with optional axis config. | PENDING |
| 43 | `create_blend_space_1d` | Create BS1D, verify asset. | PENDING |
| 44 | `create_aim_offset` | Create AO, verify Yaw(-180,180)/Pitch(-90,90) defaults. | PENDING |
| 45 | `create_aim_offset_1d` | Create AO1D, verify asset. | PENDING |
| 46 | `create_composite` | Create composite, verify skeleton assigned. | PENDING |
| 47 | `create_pose_search_schema` | Create schema with skeleton, verify default channels (Pose+Trajectory) added. | PENDING |
| 48 | `create_anim_blueprint` | Create ABP, verify skeleton on ABP + GeneratedClass + SkeletonGeneratedClass. | PENDING |
| 49 | `create_pose_search_database` | Create DB, assign schema, verify link. | PENDING |
| 50 | `compare_skeletons` | Compare two different skeletons, verify matching/missing/extra bones. Compare same skeleton, verify 100% match. | PENDING |

---

## Animation Expansion Wave 2: Sequence Props + Sync Markers (7 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 51 | `set_sequence_properties` | Set rate_scale, bLoop, interpolation on a sequence. Read back via get_sequence_info to verify. | PENDING |
| 52 | `set_additive_settings` | Set AdditiveAnimType to LocalSpaceBase, verify DDC rebuild triggered. Test with RefPoseSeq. | PENDING |
| 53 | `set_compression_settings` | Assign bone/curve compression settings, verify via get_sequence_info. Clear with empty string. | PENDING |
| 54 | `get_sync_markers` | Read markers from a sequence with known sync markers. Verify name, time, track_index. | PENDING |
| 55 | `add_sync_marker` | Add marker at specific time, verify via get_sync_markers. Verify Guid is set. | PENDING |
| 56 | `remove_sync_marker` | Remove by name, verify gone. Remove by index, verify gone. | PENDING |
| 57 | `rename_sync_marker` | Rename marker, verify old name gone, new name present with same time. | PENDING |

---

## Animation Expansion Wave 3: Batch Ops + Montage + Poses (6 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 58 | `batch_execute` | Execute 3+ ops in one call (e.g., create_blend_space + add_blendspace_sample + set_blend_space_axis). Verify all succeed. Test with one invalid op — verify partial results. | PENDING |
| 59 | `add_montage_anim_segment` | Add anim segment to montage slot. Verify auto-calculated StartPos. Verify SetAnimReference works. | PENDING |
| 60 | `clone_notify_setup` | Clone notifies from source to target sequence. Verify count matches. Test auto_scale with different length assets. | PENDING |
| 61 | `bulk_add_notify` | Add same notify to 3+ assets. Test normalized time mode. Verify each asset has its own notify instance. | PENDING |
| 62 | `create_montage_from_sections` | Create full montage in one call with sections, flow, blend, notifies. Verify all components present. | PENDING |
| 63 | `build_sequence_from_poses` | Build sequence from per-frame bone transforms. Verify bone track count, frame count, key values match input. | PENDING |

---

## Animation Expansion Wave 4: Notify Props + PoseSearch Writes (7 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 64 | `set_notify_properties` | Set a UPROPERTY on a notify (e.g., Sound on PlaySound notify). Verify via reflection readback. Test invalid property name error. | PENDING |
| 65 | `set_database_sequence_properties` | Set bEnabled=false, MirrorOption, SamplingRange on a DB entry. Verify values stuck. | PENDING |
| 66 | `add_schema_channel` | Add a Velocity channel to a schema. Verify channel count increased. Set weight. | PENDING |
| 67 | `remove_schema_channel` | Remove channel by index. Verify count decreased. | PENDING |
| 68 | `set_channel_weight` | Set weight on existing channel. Verify value. | PENDING |
| 69 | `rebuild_pose_search_index` | Trigger rebuild on a database with sequences. Verify no crash, success returned. | PENDING |
| 70 | `set_database_search_mode` | Set PoseSearchMode to PCAKDTree, adjust KDTree params. Verify values. | PENDING |

---

## Animation Expansion Wave 5: Physics Assets + IK Chains (6 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 71 | `get_physics_asset_info` | Read a physics asset, verify bodies (bone names, mass, physics type), constraints (bone pairs, limits). | PENDING |
| 72 | `set_body_properties` | Set mass, physics type, damping on a body by bone name. Verify via get_physics_asset_info. | PENDING |
| 73 | `set_constraint_properties` | Set swing/twist limits by bone pair. Verify via get_physics_asset_info. Test index-based lookup too. | PENDING |
| 74 | `add_retarget_chain` | Add chain with start/end bones. Verify via get_ikrig_info. | PENDING |
| 75 | `remove_retarget_chain` | Remove chain by name. Verify gone. | PENDING |
| 76 | `set_retarget_chain_bones` | Update start/end bones and rename chain. Verify changes. | PENDING |

---

## Animation Expansion Wave 6: Control Rig Graph (3 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 77 | `get_control_rig_graph` | Read a Control Rig graph. Verify nodes, pins (name, direction, type, connections), and links returned. | PENDING |
| 78 | `add_control_rig_node` | Add a RigUnit node (e.g., RigUnit_SetTransform). Verify node appears in graph. Set pin defaults. | PENDING |
| 79 | `connect_control_rig_pins` | Wire two node pins. Verify link appears in get_control_rig_graph. | PENDING |

---

## Animation Expansion Wave 7: ABP Graph Wiring (3 actions)

| # | Action | Test Plan | Status |
|---|--------|-----------|--------|
| 80 | `add_anim_graph_node` | Add SequencePlayer to a state. Verify node spawned with correct type. Test BlendSpacePlayer, TwoWayBlend too. | PENDING |
| 81 | `connect_anim_graph_pins` | Wire SequencePlayer Pose output to state Result input. Verify connection. Test error when nodes in different graphs. | PENDING |
| 82 | `set_state_animation` | Set animation on a state (high-level shortcut). Verify correct player node type auto-selected and wired. | PENDING |

---

## Total: 82 tests pending

Priority order:
1. Material Function Suite (#1-11) — directly addresses GitHub issue #7
2. Animation Expansion Waves 1-3 (#42-63) — new features, low-risk
3. Animation Expansion Waves 4-5 (#64-76) — medium complexity
4. Animation Expansion Waves 6-7 (#77-82) — high complexity, experimental
5. Editor fixes (#18-20) — ship-blocking correctness
6. PR #8 contributions (#12-17) — verify community code in our build
7. Niagara Phase 3-6B (#21-41) — new features, lower urgency
