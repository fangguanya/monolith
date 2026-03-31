---
name: unreal-ai
description: Use when working with Unreal Engine AI systems via Monolith MCP — Behavior Trees, Blackboards, EQS, AI Perception, Navigation, Crowd Manager, AI Controllers, and StateTree. Triggers on AI, behavior tree, blackboard, EQS, perception, navmesh, navigation, crowd, state tree, AI controller.
---

# Unreal AI Workflows

You have access to **Monolith** with **54 AI actions** across 8 categories via the `ai` namespace.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "ai" })
```

## Key Parameter Names

- `asset_path` — the Blueprint or asset path for BehaviorTree, Blackboard, EQS, AI Controller, or StateTree
- `node_index` — zero-based index of a BT node
- `key_name` — Blackboard key name
- `sense_class` — Perception sense class: Sight, Hearing, Damage, Touch, Team
- `state_name` — StateTree state name

## Action Reference

### Behavior Trees (14)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_behavior_tree_info` | asset_path | Get BT structure and nodes |
| `list_behavior_trees` | path_filter? | List all BT assets |
| `get_bt_node_details` | asset_path, node_index | Get specific node details |
| `add_bt_node` | asset_path, node_class, parent_index? | Add a node to BT |
| `remove_bt_node` | asset_path, node_index | Remove a node |
| `move_bt_node` | asset_path, node_index, new_parent_index, new_child_order | Move node in tree |
| `add_bt_decorator` | asset_path, node_index, decorator_class | Add decorator to node |
| `remove_bt_decorator` | asset_path, node_index, decorator_index | Remove decorator |
| `add_bt_service` | asset_path, node_index, service_class | Add service to node |
| `remove_bt_service` | asset_path, node_index, service_index | Remove service |
| `get_bt_decorator_info` | asset_path, node_index, decorator_index | Get decorator details |
| `get_bt_service_info` | asset_path, node_index, service_index | Get service details |
| `set_bt_node_property` | asset_path, node_index, property_name, value | Set node property |
| `validate_behavior_tree` | asset_path | Validate BT for errors |

### Blackboard (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_blackboard_keys` | asset_path | List all keys |
| `add_blackboard_key` | asset_path, key_name, key_type | Add a key |
| `remove_blackboard_key` | asset_path, key_name | Remove a key |
| `set_blackboard_key_property` | asset_path, key_name, property_name, value | Set key property |
| `get_blackboard_info` | asset_path | Get BB metadata |
| `list_blackboard_assets` | path_filter? | List all BB assets |

### EQS (7)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_eqs_query_info` | asset_path | Get EQS query details |
| `list_eqs_queries` | path_filter? | List all EQS assets |
| `add_eqs_test` | asset_path, test_class | Add a test |
| `remove_eqs_test` | asset_path, test_index | Remove a test |
| `add_eqs_generator` | asset_path, generator_class | Add generator |
| `set_eqs_test_property` | asset_path, test_index, property_name, value | Set test property |
| `get_eqs_generator_info` | asset_path | Get generator info |

### AI Controller (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_ai_controller_info` | asset_path | Get controller config |
| `set_behavior_tree` | asset_path, bt_path | Assign BT to controller |
| `set_blackboard_asset` | asset_path, bb_path | Assign BB to controller |

### Navigation (4)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_navmesh_info` | — | Get NavMesh config |
| `set_nav_area` | asset_path, area_class | Set nav area |
| `get_nav_agent_config` | — | Get nav agent properties |
| `rebuild_navigation` | — | Rebuild nav |

### Crowd Manager (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_crowd_manager_config` | — | Get crowd config |
| `set_crowd_manager_property` | property_name, value | Set crowd property |

### AI Perception (7)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_perception_config` | asset_path | Get perception setup |
| `set_perception_sense` | asset_path, sense_class, config? | Add/update sense |
| `remove_perception_sense` | asset_path, sense_class | Remove sense |
| `get_perception_sense_details` | asset_path, sense_class | Get sense details |
| `list_perception_sense_classes` | — | List available sense types |
| `set_sight_config` | asset_path, sight_radius?, lose_sight_radius?, peripheral_vision_angle? | Configure sight |
| `set_hearing_config` | asset_path, hearing_range?, max_age? | Configure hearing |

### StateTree (10) — Conditional: requires StateTree plugin

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_state_tree_info` | asset_path | Get StateTree structure |
| `list_state_trees` | path_filter? | List all StateTree assets |
| `list_state_tree_states` | asset_path | List all states |
| `add_state_tree_state` | asset_path, state_name, parent_state? | Add a state |
| `remove_state_tree_state` | asset_path, state_name | Remove a state |
| `add_state_tree_task` | asset_path, state_name, task_class | Add task to state |
| `set_state_tree_transition` | asset_path, source_state, target_state, trigger? | Set transition |
| `get_state_tree_binding` | asset_path, state_name? | Get bindings |
| `set_state_tree_binding` | asset_path, state_name, source_path, target_path | Set binding |
| `validate_state_tree` | asset_path | Validate StateTree |

### General (1)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_ai_assets` | type?, path_filter? | List AI assets by type |

## Common Workflows

### Create a new AI setup
1. Create Blackboard: use blueprint actions to create asset
2. `add_blackboard_key` — add keys (TargetActor, PatrolLocation, etc.)
3. Create BehaviorTree asset
4. `add_bt_node` — build the tree structure
5. `add_bt_decorator` / `add_bt_service` — add conditions & services
6. `set_behavior_tree` + `set_blackboard_asset` — assign to AI controller

### Configure Perception
1. `get_perception_config` — check current setup
2. `set_sight_config` — configure vision
3. `set_hearing_config` — configure hearing
4. `set_perception_sense` — add damage/touch/team senses
