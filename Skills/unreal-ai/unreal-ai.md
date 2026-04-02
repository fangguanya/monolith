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

## Action Categories

| Category | Count | Scope |
|----------|-------|-------|
| Behavior Trees | 14 | BT structure, nodes, decorators, services |
| Blackboard | 6 | Keys, metadata, assets |
| EQS | 7 | Queries, tests, generators |
| AI Controller | 3 | BT/BB assignment |
| Navigation | 4 | NavMesh, nav areas, agents |
| Crowd Manager | 2 | Crowd config |
| AI Perception | 7 | Senses (sight, hearing, damage, touch, team) |
| StateTree | 10 | States, tasks, transitions, bindings (conditional) |
| General | 1 | Asset discovery |

## Common Workflows

### Create a new AI setup
1. `add_blackboard_key` — add keys (TargetActor, PatrolLocation, etc.)
2. `add_bt_node` — build the BehaviorTree structure
3. `add_bt_decorator` / `add_bt_service` — add conditions & services
4. `set_behavior_tree` + `set_blackboard_asset` — assign to AI controller

### Configure Perception
1. `get_perception_config` — check current setup
2. `set_sight_config` — configure vision
3. `set_hearing_config` — configure hearing
4. `set_perception_sense` — add damage/touch/team senses
