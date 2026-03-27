---
name: unreal-mesh
description: Use when working with Unreal Engine meshes, scene spatial queries, level blockout, actor manipulation, 3D awareness, horror spatial analysis, accessibility validation, GeometryScript mesh operations, lighting analysis, audio/acoustics, performance budgeting, or decal/detail placement via Monolith MCP. Triggers on mesh, StaticMesh, SkeletalMesh, blockout, spatial, raycast, overlap, scene, actor, spawn, LOD, collision, UV, triangle, bounds, scan volume, scatter, navmesh, sightline, hiding, horror, tension, accessibility, wheelchair, lighting, dark, audio, acoustic, surface, footstep, reverb, performance, budget, draw call, decal, blood trail.
---

# Unreal Mesh & Spatial Workflows

You have access to **Monolith** with **73+ Mesh actions** (Phases 1-6 compiled, 7-10 in progress) via `mesh_query()`.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "mesh" })
```

## Key Parameter Names

- `asset_path` — mesh asset path for inspection actions
- `actor_name` — placed level actor name (label or internal name)
- `volume_name` — name of a BlockingVolume with Monolith.Blockout tag
- `handle` — named mesh handle (Phase 5, GeometryScript only)

## Action Reference

### Mesh Inspection (12 actions) — Read-only, query any StaticMesh/SkeletalMesh

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_mesh_info` | `asset_path` | Tri/vert count, bounds, materials, LODs, collision, Nanite, vertex colors |
| `get_mesh_bounds` | `asset_path` | AABB, extent, sphere radius, volume (cm³), surface area |
| `get_mesh_materials` | `asset_path` | Per-section material paths + tri counts |
| `get_mesh_lods` | `asset_path` | Per-LOD tri/vert counts + screen sizes |
| `get_mesh_collision` | `asset_path` | Collision type, shape counts (box/sphere/capsule/convex) |
| `get_mesh_uvs` | `asset_path`, `lod_index`?, `uv_channel`? | UV channels, island count, overlap % |
| `analyze_skeletal_mesh` | `asset_path` | Quality analysis: bone weights, degenerate tris, LOD delta |
| `analyze_mesh_quality` | `asset_path` | Non-manifold edges, degenerate tris, loose verts, quality score |
| `compare_meshes` | `asset_path_a`, `asset_path_b` | Side-by-side delta with percentages |
| `get_vertex_data` | `asset_path`, `offset`?, `limit`? | Paginated vertex positions + normals (max 5000) |
| `search_meshes_by_size` | `min_bounds`, `max_bounds`, `category`? | Find meshes by dimension range from catalog |
| `get_mesh_catalog_stats` | — | Total indexed meshes, category + size breakdown |

### Scene Manipulation (8 actions) — Actor CRUD on placed level actors

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_actor_info` | `actor_name` | Class, transform, mesh, materials, mobility, tags, components |
| `spawn_actor` | `class_or_mesh`, `location`, `rotation`?, `scale`?, `name`? | Spawn StaticMeshActor (path starts with `/`) or class |
| `move_actor` | `actor_name`, `location`?, `rotation`?, `scale`?, `relative`? | Set or offset actor transform |
| `duplicate_actor` | `actor_name`, `new_name`?, `offset`? | Clone actor with optional offset |
| `delete_actors` | `actor_names` | Delete placed actors (NOT asset files) |
| `group_actors` | `actor_names`, `group_name` | Move actors to folder |
| `set_actor_properties` | `actor_name`, `mobility`?, `simulate_physics`?, `tags`? | Set mobility, physics, shadows, tags, mass |
| `batch_execute` | `actions` | Multiple actions in single undo transaction (cap 200) |

### Scene Spatial Queries (11 actions) — Physics-based world queries

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `query_raycast` | `start`, `end`, `channel`? | Line trace — hit actor, location, normal, distance, phys material |
| `query_multi_raycast` | `start`, `end`, `max_hits`? | Multi-hit trace sorted by distance |
| `query_radial_sweep` | `origin`, `radius`?, `ray_count`?, `vertical_angles`? | Sonar sweep — compass direction summary (cap 512 rays) |
| `query_overlap` | `location`, `shape`, `extent` | Overlap test (box/sphere/capsule) |
| `query_nearest` | `location`, `class_filter`?, `tag_filter`?, `radius`? | Find nearest actors (broadphase) |
| `query_line_of_sight` | `from`, `to` | Visibility check — bool + blocking actor |
| `get_actors_in_volume` | `volume_name` | All actors in a named volume |
| `get_scene_bounds` | `class_filter`? | World AABB enclosing all actors |
| `get_scene_statistics` | `region_min`?, `region_max`? | Actor counts, total tris, lights, navmesh status |
| `get_spatial_relationships` | `actor_name`, `radius`?, `limit`? | Neighbors with relationships (on_top_of, adjacent, near...) |
| `query_navmesh` | `start`, `end`, `agent_radius`? | Navigation path query — points, distance, reachable |

### Level Blockout (15 actions) — Tag-based blockout volumes, asset matching, replacement

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_blockout_volumes` | — | List all Monolith.Blockout tagged volumes |
| `get_blockout_volume_info` | `volume_name` | Volume details, primitive list, other actors |
| `setup_blockout_volume` | `volume_name`, `room_type`, `tags`?, `density`? | Apply Monolith tags to a BlockingVolume |
| `create_blockout_primitive` | `shape`, `location`, `scale`, `label`?, `volume_name`? | Spawn tagged blockout primitive |
| `create_blockout_primitives_batch` | `primitives`, `volume_name`? | Batch placement (cap 200, single undo) |
| `create_blockout_grid` | `volume_name`, `cell_size` | Floor grid in volume |
| `match_asset_to_blockout` | `blockout_actor`, `tolerance_pct`?, `top_n`? | Find size-matched assets from catalog |
| `match_all_in_volume` | `volume_name`, `tolerance_pct`?, `top_n`? | Batch match all primitives in volume |
| `apply_replacement` | `replacements` | Atomic replace blockouts with real assets (pivot-adjusted) |
| `set_actor_tags` | `actor_tags` | Batch apply tags |
| `clear_blockout` | `volume_name`, `keep_tagged`? | Remove blockout primitives by owner tag |
| `export_blockout_layout` | `volume_name` | Export as JSON (positions relative 0-1, sizes absolute) |
| `import_blockout_layout` | `volume_name`, `layout_json` | Import layout (scales positions, keeps sizes) |
| `scan_volume` | `volume_name`, `ray_density`? | Daredevil scan — walls, floor, ceiling, openings |
| `scatter_props` | `volume_name`, `asset_paths`, `count`, `min_spacing`?, `seed`? | Poisson disk scatter with collision avoidance |

### Mesh Operations (12 actions) — `#if WITH_GEOMETRYSCRIPT`, require handles

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_handle` | `source`, `handle` | Load mesh into editable handle (asset path or "primitive:box") |
| `release_handle` | `handle` | Free a handle |
| `list_handles` | — | All handles with tri count, last access time |
| `save_handle` | `handle`, `target_path`, `overwrite`? | Commit handle to new StaticMesh asset |
| `mesh_boolean` | `handle_a`, `handle_b`, `operation`, `result_handle` | Union/subtract/intersect |
| `mesh_simplify` | `handle`, `target_triangles`? | Reduce triangle count |
| `mesh_remesh` | `handle`, `target_edge_length` | Isotropic remeshing |
| `generate_collision` | `handle`, `method` | Convex decomp/auto shapes |
| `generate_lods` | `handle`, `lod_count` | LOD chain via repeated simplification |
| `fill_holes` | `handle` | Automatic hole detection + fill |
| `compute_uvs` | `handle`, `method` | Auto-unwrap/box/planar/cylinder projection |
| `mirror_mesh` | `handle`, `axis` | Mirror across X/Y/Z |

### Horror Spatial Analysis (8 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `analyze_sightlines` | `location`, `fov`?, `ray_count`? | Claustrophobia score, blocked % at 5/10/20m |
| `find_hiding_spots` | `region_min/max`, `viewpoints` | Grid-sample concealment from viewpoints |
| `find_ambush_points` | `path_points`, `lateral_range`? | Concealed positions with surprise angles |
| `analyze_choke_points` | `start`, `end` | Navmesh path width, flank possibility |
| `analyze_escape_routes` | `location`, `exit_tags`? | Paths to exits scored by threat exposure |
| `classify_zone_tension` | `location`, `radius`? | Calm/uneasy/tense/dread/panic composite |
| `analyze_pacing_curve` | `path_points` | Tension along path, scare point detection |
| `find_dead_ends` | `region_min/max`? | Navmesh flood-fill for single-exit regions |

### Accessibility Analysis (6 actions) — Serves the hospice mission

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `validate_path_width` | `start`, `end`, `min_width`? | Wheelchair clearance (120cm default) |
| `validate_navigation_complexity` | `start`, `end` | Cognitive difficulty scoring |
| `analyze_visual_contrast` | `location`, `forward`? | WCAG-inspired contrast for interactables |
| `find_rest_points` | `start`, `end`, `max_gap`? | Safe room spacing along path |
| `validate_interactive_reach` | `region`, `tags`? | Height/distance/obstruction checks |
| `generate_accessibility_report` | `start`, `end`, `profile`? | Motor/vision/cognitive profile, A-F grade |

### Lighting Analysis (5 actions) — Hybrid scene capture + analytic

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `sample_light_levels` | `points`, `mode`? | Per-point luminance via scene capture or analytic |
| `find_dark_corners` | `volume_name` or `region`, `threshold`? | Ortho capture + flood-fill dark zones |
| `analyze_light_transitions` | `path_points`, `sample_interval`? | Flag harsh bright→dark (>4:1 ratio) |
| `get_light_coverage` | `volume_name` | % lit/shadow/dark, light inventory, histogram |
| `suggest_light_placement` | `volume_name`, `mood`? | Inverse-square placement for horror_dim/safe_room/clinical |

### Audio & Acoustics (14 actions) — Physical material-aware spatial audio

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_audio_volumes` | — | Enumerate AudioVolumes with reverb settings |
| `get_surface_materials` | `volume_name` or `region` | Physical material breakdown (% concrete, tile, etc.) |
| `estimate_footstep_sound` | `location` | Surface type → loudness factor (0-1) |
| `analyze_room_acoustics` | `volume_name` | Sabine RT60 reverb estimation from surfaces |
| `analyze_sound_propagation` | `from`, `to` | Material-based occlusion, wall count, dB reduction |
| `find_loud_surfaces` | `volume_name` or `region` | Dangerous surfaces (metal, glass, gravel) |
| `find_sound_paths` | `from`, `to`, `max_bounces`? | Image-source reflection paths with attenuation |
| `can_ai_hear_from` | `ai_location`, `player_location` | Monster hearing detection with surface awareness |
| `get_stealth_map` | `volume_name`, `grid_size`? | Per-cell loudness + detection radius heatmap |
| `find_quiet_path` | `start`, `end` | Navmesh path preferring quiet surfaces |
| `suggest_audio_volumes` | `volume_name` | Auto-suggest reverb settings from room materials |
| `create_audio_volume` | `volume_name`, `reverb_preset` | Spawn AudioVolume with suggested settings |
| `set_surface_type` | `actor_name`, `surface_type` | Set physical material surface override |
| `create_surface_datatable` | `template`? | Bootstrap acoustic system (12 horror surfaces) |

### Performance Analysis (5 actions) — Budget-aware placement

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_region_performance` | `region_min`, `region_max` | Tri count, draw calls, lights, shadow casters |
| `estimate_placement_cost` | `assets` (array of {path, count}) | Pre-spawn budgeting without spawning |
| `find_overdraw_hotspots` | `viewpoint`, `fov`? | Translucent screen-space overlap |
| `analyze_shadow_cost` | `region_min`, `region_max` | Flag small props casting shadows pointlessly |
| `get_triangle_budget` | `viewpoint`, `fov`?, `budget`? | Frustum-culled LOD-aware count vs budget |

### Decal & Detail Placement (4 actions) — Environmental storytelling

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `place_decals` | `material`, `locations` or `region`+`count` | Surface-aligned decals, Poisson scatter |
| `place_along_path` | `path_points`, `pattern`? | Blood trails, footprints, drag marks (Catmull-Rom) |
| `analyze_prop_density` | `volume_name`, `target_density`? | Grid-cell density vs target |
| `place_storytelling_scene` | `location`, `pattern`, `intensity`? | 5 horror presets (violence/abandoned/dragged/medical/corruption) |

## Blockout Tag Convention

Blockout uses standard actor tags (`TArray<FName>`) on `ABlockingVolume` — zero runtime footprint:

```
Monolith.Blockout                — sentinel (required)
Monolith.Room:Kitchen            — room type
Monolith.Tag:Furniture.Kitchen   — asset matching tag (one per entry)
Monolith.Density:Normal          — Sparse/Normal/Dense/Cluttered
Monolith.AllowPhysics            — presence = true
Monolith.Owner:BV_Kitchen        — on primitives, links to volume
Monolith.BlockoutPrimitive       — marks as blockout primitive
Monolith.Label:Counter_North     — human-readable label
```

## Typical Workflows

### Blockout a Room
```
get_blockout_volumes → scan_volume → create_blockout_primitives_batch → match_all_in_volume → apply_replacement
```

### Inspect a Mesh
```
get_mesh_info → analyze_mesh_quality → compare_meshes
```

### Edit a Mesh (GeometryScript)
```
create_handle → [mesh_simplify | mesh_boolean | compute_uvs | ...] → save_handle → release_handle
```

### Horror Level Design
```
analyze_sightlines → find_hiding_spots → analyze_escape_routes → classify_zone_tension → analyze_pacing_curve
```

### Accessibility Check
```
generate_accessibility_report (or individual validate_ actions)
```

### Horror Audio Design
```
create_surface_datatable (first time) → get_surface_materials → analyze_room_acoustics → get_stealth_map → can_ai_hear_from → suggest_audio_volumes
```

### Lighting Audit
```
get_light_coverage → find_dark_corners → analyze_light_transitions → suggest_light_placement
```

### Performance Budget
```
get_region_performance → get_triangle_budget → analyze_shadow_cost → find_overdraw_hotspots
```

### Environmental Storytelling
```
place_storytelling_scene → place_along_path → scatter_props → analyze_prop_density
```

### Understand a Scene
generate_accessibility_report (or individual validate_ actions)
```

### Understand a Scene
```
get_scene_statistics → query_radial_sweep → get_spatial_relationships
```

## Gotchas

- `spawn_actor` does NOT spawn `ABlockingVolume` — use the editor for volumes
- `delete_actors` deletes placed actors, NOT asset files (use `editor_query("delete_assets")` for that)
- `batch_execute` rejects nested `batch_execute` and caps at 200 actions
- `set_actor_properties`: Mobility must be "Movable" BEFORE enabling SimulatePhysics
- `query_radial_sweep` hard cap: `ray_count * vertical_angles <= 512`
- `search_meshes_by_size` requires `monolith_reindex()` to have populated the mesh catalog first
- All spatial queries work in editor WITHOUT a play session
- `query_` prefix = active physics queries. `get_` prefix = reads stored data
