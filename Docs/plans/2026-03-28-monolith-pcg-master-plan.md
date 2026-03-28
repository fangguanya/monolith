# MonolithPCG Module + Modular Kit Scanner -- Master Implementation Plan

> **For agentic workers:** This is a MASTER PLAN defining 6 sub-plans across 4 delivery milestones. Each sub-plan has independent work packages (WPs) that can be dispatched to implementation agents in parallel. Dependencies between sub-plans are explicit. The killer feature (Sub-Plan B: Kit Scanner) has highest priority and should be started first.

**Goal:** Add MonolithPCG as module #12 to the Monolith MCP plugin, providing a `pcg_query(action, params)` namespace for programmatic PCG graph construction, execution, and management. The crown jewel is `scan_modular_kit` -- point at a folder of modular meshes, auto-classify them, detect grid/openings/pivots, generate missing pieces, and build entire towns from the user's own art.

**Architecture:** Three-layer hybrid (established in research):
1. **GeometryScript** -- mesh vocabulary generation (unique shells, fallback pieces, terrain-adaptive foundations)
2. **Floor Plan System** -- the orchestrator (our existing SP2 treemap + adjacency, spatial registry, building descriptors)
3. **PCG** -- placement and instancing (furniture scatter, horror dressing, vegetation, street furniture, all via ISM/HISM)

**Tech Stack:** UE 5.7 (PCG Production-Ready), C++ (new MonolithPCG module), GeometryScript (fallback piece generation), JSON (kit configs, presets), PCGEx (MIT, optional for road networks).

**Estimated Total:** ~800-1080h across all sub-plans (multi-month roadmap).

---

## Table of Contents

1. [Delivery Milestones](#delivery-milestones)
2. [Sub-Plan A: MonolithPCG Module Foundation](#sub-plan-a-monolithpcg-module-foundation)
3. [Sub-Plan B: Modular Kit Scanner (Killer Feature)](#sub-plan-b-modular-kit-scanner-killer-feature)
4. [Sub-Plan C: PCG Town Dressing](#sub-plan-c-pcg-town-dressing)
5. [Sub-Plan D: PCG Gameplay Integration](#sub-plan-d-pcg-gameplay-integration)
6. [Sub-Plan E: Terrain + Landscape](#sub-plan-e-terrain--landscape)
7. [Sub-Plan F: Runtime + Streaming](#sub-plan-f-runtime--streaming)
8. [Cross-Cutting Concerns](#cross-cutting-concerns)
9. [File Conflict Analysis](#file-conflict-analysis)
10. [Risk Register](#risk-register)

---

## Delivery Milestones

```
Milestone 1: "I can build PCG graphs from MCP and scan any modular kit"
  Sub-Plan A (Foundation) + Sub-Plan B (Kit Scanner)
  Duration: ~8-12 weeks
  Actions added: ~60-75

Milestone 2: "Generated towns have vegetation, horror dressing, and atmosphere"
  Sub-Plan C (Town Dressing)
  Duration: ~6-10 weeks (can overlap M1 tail)
  Actions added: ~25-35

Milestone 3: "Gameplay elements are procedurally placed"
  Sub-Plan D (Gameplay Integration)
  Duration: ~5-8 weeks
  Actions added: ~20-25

Milestone 4: "Towns stream at runtime and adapt to terrain"
  Sub-Plan E (Terrain) + Sub-Plan F (Runtime)
  Duration: ~8-12 weeks
  Actions added: ~20-30
```

### Inter-Sub-Plan Dependencies

```
Sub-Plan A ──────────────────────────────────┐
  (PCG Foundation)                           │
  Must complete Phase A1 before any          │
  other sub-plan can use PCG                 │
                                             │
Sub-Plan B ──────────────────────────────────┤
  (Kit Scanner)                              │
  Independent of A for scanning/classify.    │
  Needs A Phase 1 for build_with_kit         │
  (PCG placement of scanned pieces)          │
                                             │
Sub-Plan C ──── requires A Phase 1-2 ────────┤
  (Town Dressing)                            │
                                             │
Sub-Plan D ──── requires A Phase 1-2, C ─────┤
  (Gameplay)                                 │
                                             │
Sub-Plan E ──── independent of PCG ──────────┤
  (Terrain)    (uses landscape APIs, not PCG)│
  PCG reads terrain after E modifies it      │
                                             │
Sub-Plan F ──── requires A, B, C complete ───┘
  (Runtime/Streaming)
```

---

## Sub-Plan A: MonolithPCG Module Foundation

**Estimated effort:** 80-110h
**Research docs:** `pcg-framework-research`, `pcg-mcp-integration-research`, `hybrid-gs-pcg-research`
**Deliverable:** New `MonolithPCG` module with `pcg_query` namespace, ~35 actions for graph CRUD, execution, templates, and spatial registry bridge.

### Architecture Decision Record

The module is a **graph construction + execution bridge**, not a PCG extension toolkit. Monolith's role is orchestration via MCP -- agents construct PCG graphs programmatically from built-in UE nodes, set parameters, trigger execution, and read results. Custom PCG nodes (UPCGSettings subclasses) are limited to 2-3 data bridge nodes for reading our spatial registry.

Key API surfaces (all verified in engine source):
- `UPCGGraph::AddNodeOfType()`, `AddEdge()`, `SetGraphParameter<T>()` -- graph construction
- `UPCGComponent::GenerateLocal()`, `SetGraph()` -- execution
- `UPCGSubsystem::ScheduleComponent()` -- low-level scheduling
- `FInstancedPropertyBag` (StructUtils) -- user parameters
- `UPCGPointData`, `UPCGPolygon2DData`, `UPCGParamData` -- data types
- PCG nodes auto-register via UE reflection (no manual registration)

### Phase A1: Module Bootstrap + Graph CRUD (25-30h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| A1.1 | Create MonolithPCG module scaffold | `Source/MonolithPCG/MonolithPCG.Build.cs`, `Public/MonolithPCGModule.h`, `Private/MonolithPCGModule.cpp` | Follow MonolithMesh pattern. Build.cs: depend on `PCG`, `MonolithCore`, `MonolithIndex`, `StructUtils`, `Json`, `JsonUtilities`, `AssetRegistry`, `AssetTools`, `UnrealEd`. Optional `PCGEditor` if editor build. WITH_PCG define gated like WITH_GEOMETRYSCRIPT. | Compiles cleanly with UBT. Module loads in editor log. | 4h |
| A1.2 | Register `pcg_query` namespace in MonolithCore | `MonolithPCGModule.cpp`, minor edit to `MonolithCore` registration | Register namespace "pcg" with tool name "pcg_query" in `FMonolithToolRegistry`. Follow pattern of MonolithMesh registration. | `monolith_discover("pcg")` returns action list. | 3h |
| A1.3 | Add module + plugin deps to uplugin | `Monolith.uplugin` | Add `MonolithPCG` module entry (Type: Editor, LoadingPhase: Default). Add `PCG` plugin dependency (Enabled: true, Optional: true). | Plugin loads without errors. PCG available. | 1h |
| A1.4 | Implement graph CRUD actions (8 actions) | `Public/MonolithPCGActions.h`, `Private/MonolithPCGActions.cpp` | Actions: `create_pcg_graph`, `list_pcg_graphs`, `get_pcg_graph_info`, `add_pcg_node`, `remove_pcg_node`, `connect_pcg_nodes`, `disconnect_pcg_nodes`, `set_pcg_node_params`. For `add_pcg_node`: maintain a `TMap<FString, TSubclassOf<UPCGSettings>>` mapping friendly names ("SurfaceSampler", "StaticMeshSpawner", "DensityFilter", etc.) to settings classes. Use `UPCGGraph::AddNodeOfType()`. For connections: `UPCGGraph::AddEdge(FromNode, FromPin, ToNode, ToPin)`. For params: use `FJsonObjectConverter::JsonObjectToUStruct` to apply JSON properties to UPCGSettings UPROPERTY fields. | Can create a graph, add 3 nodes (Surface Sampler -> Density Filter -> Static Mesh Spawner), connect them, and read back the graph structure. | 14h |
| A1.5 | PCG settings class resolver | `Private/MonolithPCGSettingsResolver.h/.cpp` | Scan all UPCGSettings subclasses at module startup via `GetDerivedClasses()`. Build friendly-name -> class map stripping "UPCG" prefix and "Settings" suffix (e.g., `UPCGSurfaceSamplerSettings` -> "SurfaceSampler"). Cache as TMap. Expose `list_pcg_node_types` action. | `list_pcg_node_types` returns 50+ node types. `add_pcg_node({type: "SurfaceSampler"})` resolves correctly. | 5h |

**Done criteria for Phase A1:** An agent can create a PCG graph asset, add nodes, wire them, set parameters, and query the result -- all via MCP.

### Phase A2: Execution + Scene Integration (20-25h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| A2.1 | Parameter + execution actions (7 actions) | `Public/MonolithPCGExecutionActions.h`, `Private/MonolithPCGExecutionActions.cpp` | Actions: `add_pcg_parameter`, `set_pcg_parameter`, `get_pcg_parameters`, `execute_pcg`, `cleanup_pcg`, `get_pcg_output`, `cancel_pcg_execution`. Parameters via `UPCGGraph::AddUserParameters()` (FPropertyBagPropertyDesc) and `SetGraphParameter<T>()`. Execution via `UPCGComponent::GenerateLocal(bForce)`. Output via `GetGeneratedGraphOutput()` returning point counts, spawned actor names, ISM instance counts. | Set a float param, execute, read output showing spawned meshes. | 12h |
| A2.2 | Scene integration actions (7 actions) | `Public/MonolithPCGSceneActions.h`, `Private/MonolithPCGSceneActions.cpp` | Actions: `spawn_pcg_volume`, `spawn_pcg_component`, `set_pcg_component_graph`, `list_pcg_components`, `refresh_pcg_component`, `get_pcg_generated_actors`, `set_pcg_generation_trigger`. Spawn `APCGVolume` with SetFolderPath("Monolith/PCG"). Set generation trigger enum. List via `TActorIterator<APCGVolume>` + component scan. | Spawn a volume, assign a graph, execute, list generated actors. | 10h |

**Done criteria for Phase A2:** Full create-configure-execute-inspect loop works via MCP. An agent can create a scatter graph, place a volume, execute it, and see spawned meshes in the level.

### Phase A3: Templates + Presets (15-20h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| A3.1 | Template graph builders (8 actions) | `Public/MonolithPCGTemplateActions.h`, `Private/MonolithPCGTemplateActions.cpp` | Actions: `create_scatter_graph`, `create_spline_scatter_graph`, `create_vegetation_graph`, `create_horror_dressing_graph`, `create_debris_graph`, `apply_pcg_preset`, `save_pcg_preset`, `list_pcg_presets`. Each template builds a complete graph programmatically: e.g., `create_scatter_graph` builds `GetActorData -> SurfaceSampler -> DensityFilter -> SelfPruning -> StaticMeshSpawner` with user params for density, mesh_list, min_distance, seed. Presets serialize/deserialize to `Saved/Monolith/PCGPresets/`. | `create_scatter_graph({density: 0.5, meshes: ["/Game/Props/Chair"]})` produces a working graph that spawns chairs when executed. | 15h |

### Phase A4: Spatial Registry Bridge + Custom PCG Nodes (20-25h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| A4.1 | Bridge actions (5 actions) | `Public/MonolithPCGBridgeActions.h`, `Private/MonolithPCGBridgeActions.cpp` | Actions: `spatial_registry_to_pcg`, `pcg_output_to_registry`, `building_descriptor_to_pcg`, `create_room_scatter_graph`, `batch_execute_pcg`. `spatial_registry_to_pcg`: Read spatial registry JSON from `Saved/Monolith/SpatialRegistry/`, convert rooms to UPCGPointData (one point per room, with room_type/decay/bounds attributes) OR UPCGPolygon2DData (room boundary as closed polygon). `pcg_output_to_registry`: Read generated actors, write back to spatial registry. `batch_execute_pcg`: Execute multiple PCG components with dependency chain via `UPCGSubsystem::ScheduleComponent()`. | Convert a building with 6 rooms to PCG data, execute a room scatter graph, verify furniture placed inside room bounds. | 10h |
| A4.2 | Custom PCG data source node: FloorPlan | `Public/PCGNodes/PCGFloorPlanDataSource.h`, `Private/PCGNodes/PCGFloorPlanDataSource.cpp` | `UPCGFloorPlanSettings : UPCGSettings` + `FPCGFloorPlanElement : IPCGElement`. Settings: SpatialRegistryPath (FString), BuildingId (FString). No input pins (data source). Three output pins: "WallPoints" (Point data with wall_type, has_door, has_window, room_id, decay, floor_index attributes), "RoomPolygons" (Polygon2D data for each room boundary), "BuildingParams" (ParamData with style, floors, grid_size). Auto-discovered by PCG via reflection. | Node appears in PCG editor palette. Connect to StaticMeshSpawner, execute, see wall pieces placed at correct positions. | 8h |
| A4.3 | Custom PCG data source node: ModularKit | `Public/PCGNodes/PCGModularKitDataSource.h`, `Private/PCGNodes/PCGModularKitDataSource.cpp` | `UPCGModularKitSettings : UPCGSettings`. Settings: KitPath (FString, path to kit JSON). Output pin: "KitPieces" (ParamData with piece_type, asset_path, dimensions, opening_info per entry). Used downstream by PCG graphs to drive mesh selection based on wall_type attribute matching. | Load a scanned kit JSON, output piece catalog as PCG param data. | 5h |

**Done criteria for Phase A4:** PCG graphs can consume our spatial registry data natively. A building's rooms flow into PCG as Polygon2D data, enabling native interior surface sampling for furniture scatter.

### Phase A Summary

| Phase | Actions | Est Hours | Dependencies |
|-------|---------|-----------|--------------|
| A1: Module + Graph CRUD | 9 (+list_pcg_node_types) | 25-30h | None |
| A2: Execution + Scene | 14 | 20-25h | A1 |
| A3: Templates + Presets | 8 | 15-20h | A1, A2 |
| A4: Bridge + Custom Nodes | 5 + 2 custom PCG nodes | 20-25h | A1, A2, existing spatial registry |
| **Total** | **~37 actions** | **80-110h** | |

---

## Sub-Plan B: Modular Kit Scanner (Killer Feature)

**Estimated effort:** 120-160h
**Research docs:** `asset-scanning-research`, `mesh-opening-detection-research`, `marketplace-kit-conventions-research`, `kit-scanner-ux-research`, `grid-detection-research`, `fallback-piece-generation-research`, `modular-building-research`, `modular-pieces-research`, `hism-assembly-research`
**Deliverable:** `scan_modular_kit` and supporting actions that let a user point at ANY folder of modular meshes and immediately get classified pieces, detected grid, gap analysis, generated fallbacks, and the ability to build entire towns from their art.

### The Golden Path (3-Turn UX)

1. User: "scan /Game/HorrorKit/Meshes/ as a modular building kit"
   - System scans 47 meshes, classifies by 5-signal pipeline, detects 200cm grid, reports 78% coverage, flags 6 uncertain pieces
2. User: "SM_Panel_Large is wall_solid, SM_Arch_001 is wall_door, accept the rest"
   - System updates classifications, persists corrections, coverage rises to 82%
3. User: "generate a 4-building horror block using HorrorKit"
   - System generates buildings using scanned pieces via HISM, GeometryScript fills 4 missing piece types

### Architecture

```
scan_modular_kit
  |
  +-- Asset Registry scan (IAssetRegistry::GetAssets with FARFilter)
  +-- Per-mesh classification pipeline:
  |     1. Name parsing (regex, ~80% accuracy on well-named)
  |     2. Dimension analysis (bounds ratios, height/width/thickness)
  |     3. Material slot analysis (names like "glass", "frame" = window)
  |     4. Topology analysis (FMeshBoundaryLoops for opening detection)
  |     5. Socket detection (snap points imply modular)
  |
  +-- Grid detection (candidate scoring + histogram validation)
  +-- Pivot detection (bounds.Origin vs mesh AABB)
  +-- Coverage analysis (present vs missing piece types)
  +-- Fallback plan (GeometryScript generation for missing types)
  +-- Kit persistence (JSON at Saved/Monolith/ModularKits/{name}.json)
  |
  v
build_with_kit
  |
  +-- Read kit JSON
  +-- Read building descriptor (from existing SP1/SP2)
  +-- Map wall edges to kit pieces (wall_type -> piece_type)
  +-- Place via HISM (one HISM component per unique mesh)
  +-- Generate fallback pieces on-demand for missing types
  +-- Outliner organization (SetFolderPath)
```

### Phase B1: Asset Scanning + Name Classification (20-25h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| B1.1 | Core scanning infrastructure | `Public/MonolithPCGKitScannerActions.h`, `Private/MonolithPCGKitScannerActions.cpp` (or in separate MonolithPCG module, or extend MonolithMesh -- see file conflict analysis) | `IAssetRegistry::ScanPathsSynchronous()` + `GetAssets(FARFilter)` with `ClassPaths = UStaticMesh`. Recursive. Collect `FAssetData` array. Load each mesh for bounds/tris/materials. Return structured scan results. | Scan a folder of 30+ meshes, return all with bounds/tris/material counts in <5s. | 6h |
| B1.2 | Name parsing classifier | Same files | Regex-based classifier using marketplace conventions survey. Primary regex: `^SM_(?:Env_)?(?<tileset>\w+_)?(?<type>Wall\|Floor\|Ceiling\|Roof\|Stair\|Door\|Window\|Trim\|Column\|Pillar\|Railing\|Balcony)(?:_(?<subtype>...))?(?:_(?<size>...))?(?:_(?<variant>...))?$`. FRegexMatcher in UE. Secondary patterns for material-first naming (SM_brick_wall), tileset naming (SM_Env_Dungeon_Wall), and size encoding (_1x1, _200, _Half). Output: predicted type + confidence score (0.0-1.0). | Correctly classifies 80%+ of Synty POLYGON naming, PurePolygons naming, and Junction City naming patterns. | 6h |
| B1.3 | Dimension analysis classifier | Same files | Analyze bounds ratios: Wall = height >> width >> thickness (Z > X > Y with Y < 30cm). Floor = width ~= depth, height < 10cm. Stair = moderate aspect with stepped height. Use ratio thresholds derived from marketplace survey: wall thickness 8-25cm, wall height 240-360cm, floor thickness 3-10cm. Output: predicted type + confidence. | Correctly classifies a 200x15x300 mesh as wall_solid, 200x200x5 as floor_tile. | 5h |
| B1.4 | Material slot classifier | Same files | Scan material slot names for keywords: "glass"/"window" -> has window, "frame"/"trim" -> trim piece, "floor"/"tile" -> floor, "wall"/"brick"/"concrete" -> wall. Count of unique materials: 1 = simple piece, 3-4 = wall with openings (wall + glass + frame + trim). Boost confidence of other signals. | Material slots named "M_Glass" and "M_WoodFrame" boost window wall classification by +0.15 confidence. | 3h |
| B1.5 | Multi-signal fusion + confidence scoring | Same files | Weighted sum: name_score * 0.35 + dimension_score * 0.30 + material_score * 0.15 + topology_score * 0.15 + socket_score * 0.05. Three-tier routing: >0.80 auto-accept, 0.50-0.80 flag for review, <0.50 unclassified. Persist per-mesh classification to kit JSON. | Combined confidence exceeds individual signals. A mesh with matching name + dimensions scores >0.85. | 4h |

**Done criteria for Phase B1:** `scan_modular_kit` returns structured JSON with per-mesh classifications, confidence scores, category counts, and uncertain items flagged for review.

### Phase B2: Grid + Opening + Pivot Detection (25-35h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| B2.1 | Grid size detection | `Private/MonolithPCGGridDetection.h/.cpp` | Candidate scoring algorithm from research: Generate candidates from industry priors {50, 100, 150, 200, 250, 300, 400, 512} + data-derived (smallest dim, most common, pairwise GCDs of top-5 dims). Score each: inlier_ratio * 100 - mean_residual * 0.5 + industry_bonus + size_bonus. Tolerance 5cm default. Validate with histogram peak detection. Return grid_size + confidence. | 200cm kit detected as 200cm (not 100 or 50). 300cm kit detected correctly even with one 195cm outlier. | 8h |
| B2.2 | Mesh opening detection | `Private/MonolithPCGOpeningDetection.h/.cpp` | Pipeline: `CopyMeshFromStaticMeshV2()` -> `FDynamicMesh3` -> `FMeshBoundaryLoops::Compute()` -> filter (reject perimeter < 10cm cracks, reject loops touching mesh AABB faces) -> classify per loop by `GetBounds()` dimensions and position: Door (sill ~0, H 180-260, W 70-180), Window (sill > 60cm, H 60-180, W 40-200), Archway (sill ~0, full height), Vent (any dim < 30cm). Validate with ray-cast pass-through (grid of rays through opening center, >80% pass = confirmed). Output: `TArray<FOpeningDescriptor>` with type, bounds, centroid, normal, confidence. **Editor-time only** (source mesh required, cooked mesh has split topology). | Wall with door opening: 1 boundary loop detected, classified as door, dimensions within 5% of actual. Wall with 2 windows: 2 loops, both classified as window. Solid wall: 0 loops (closed mesh). | 12h |
| B2.3 | Pivot point detection | Same files as B1 | Analyze `FBoxSphereBounds::Origin` relative to mesh AABB. Classify: base_center (origin at bottom face center), base_corner (origin at min corner), center (origin at volume center), wall_center_bottom (origin at center-bottom of thin face). Report pivot type per mesh. Detect pivot consistency across kit. | Correctly identifies base_center pivot on PurePolygons pieces. Flags inconsistent pivots across a kit. | 4h |
| B2.4 | Socket detection | Same files as B1 | Iterate `UStaticMesh::Sockets`. Report socket names, positions, tags. Classify: "Snap_*" = connection points, "Attach_*" = decoration points. Detect if kit uses socket-based snapping vs grid-based. | Kit with sockets: reports snap points and their positions. Kit without sockets: reports "grid-based" assumption. | 3h |
| B2.5 | `classify_mesh` single-mesh action | Same action files | Expose the full classification pipeline for a single mesh. Useful for debugging and iterative correction. | `classify_mesh({asset: "/Game/Kit/SM_Wall_01"})` returns all 5 signal scores, predicted type, confidence, openings, pivot, dimensions. | 3h |

**Done criteria for Phase B2:** Grid size auto-detected. Openings detected and classified at 95%+ accuracy on well-formed kits. Pivots detected. Full per-mesh diagnostic available.

### Phase B3: Kit Management + Corrections (15-20h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| B3.1 | Kit persistence (JSON) | `Private/MonolithPCGKitPersistence.h/.cpp` | Kit config saved to `Saved/Monolith/ModularKits/{kit_name}.json`. Schema: `{kit_name, scan_path, grid_size_cm, grid_confidence, pivot_type, wall_thickness_cm, wall_height_cm, pieces: [{asset_path, piece_type, confidence, dimensions, openings, pivot, material_slots, is_user_override}], corrections: [{asset, old_type, new_type, timestamp}], coverage: {percentage, present, missing, fallback_plan}}`. Load/save via `FFileHelper::SaveStringToFile` + `FJsonSerializer`. | Save and reload a kit with 47 pieces. Corrections persist across editor restarts. | 6h |
| B3.2 | Classification correction actions | Same action files | Actions: `edit_kit_classification({kit, asset, new_type})` for single correction. `edit_kit_classification({kit, pattern: "SM_Panel*", new_type: "wall_solid"})` for batch by glob. `accept_uncertain({kit})` to auto-accept all uncertain at current predicted type. `reset_kit_classification({kit, asset?})` to re-scan. Mark corrections as `is_user_override: true` so re-scans don't overwrite. | Correct a piece from "furniture" to "wall_solid". Re-scan the kit -- user override persists. | 5h |
| B3.3 | Kit listing and management | Same action files | Actions: `list_kits` (scan Saved/Monolith/ModularKits/), `get_kit_info({kit})`, `delete_kit({kit})`, `export_kit({kit})` (copy JSON for sharing), `import_kit({json_path})`. Kit info includes coverage summary, piece counts by type, grid info. | List kits, get info for one, delete another. Export and re-import a kit JSON. | 4h |
| B3.4 | Asset metadata stamping | Same files | After classification, stamp each mesh with `UMonolithKitMetadata : UAssetUserData` containing piece_type, kit_name, grid_units. Override `GetAssetRegistryTags()` for fast future lookups without full load. Future scans check for existing metadata first (skip re-classification). | Scan a kit, verify asset tags are written. Re-scan the same kit -- metadata-tagged meshes skip classification (fast path). | 4h |

**Done criteria for Phase B3:** Kits persist as shareable JSON files. User corrections stick across sessions. Asset metadata enables fast re-scanning.

### Phase B4: Fallback Piece Generation (25-35h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| B4.1 | Dimension extraction from kit | `Private/MonolithPCGFallbackGeneration.h/.cpp` | Extract from classified kit pieces: grid_size (from B2.1), wall_thickness (min bounding dim of wall pieces, median), wall_height (max Z of wall pieces, median), floor_thickness (Z of floor pieces), door_dimensions (from opening detection on wall_door pieces, or defaults 100x230), window_dimensions (from opening detection on wall_window pieces, or defaults 80x120 at sill 90cm). Store in kit JSON as `kit_params`. | Extracts correct dimensions from a kit with 12 wall pieces and 4 floors. | 4h |
| B4.2 | P0 fallback generators (5 piece types) | Same files | Generate: wall_solid (AppendBox), floor_tile (AppendBox), ceiling_tile (AppendBox), corner_outer (L-shaped AppendSimpleExtrudePolygon), corner_inner (inverted L). All use kit_params for dimensions. MaterialID assignment per face group. Box UV projection. Save via existing `SaveMeshToAsset()` at `Saved/Monolith/ModularKits/{kit}/Fallbacks/SM_Fallback_{type}.uasset`. | Generate all 5 P0 types. Each has correct dimensions matching the kit. Each saves as valid UStaticMesh. | 8h |
| B4.3 | P1 fallback generators (5 piece types) | Same files | Generate: wall_door (wall box + Selection+Inset+Delete for door opening), wall_window (wall box + Selection+Inset+Delete for window, sill preserved), wall_t_junction (T-shaped extrusion from 2D polygon), wall_end_cap (box with one exposed end face), wall_half (box at half wall_height). Use Selection+Inset technique from modular-pieces-research -- select front face triangles in opening region, inset by bevel_width, delete inner selection. | wall_door has a boundary loop detected by opening detection (validates correct opening geometry). wall_t_junction has correct T-shaped cross-section. | 10h |
| B4.4 | P2 fallback generators (4 piece types) | Same files | Generate: floor_stairwell (floor box with rectangular cutout via Selection+Delete), stair_straight (stepped geometry via repeated AppendBox or AppendSimpleExtrudePolygon with step profile), stair_landing (flat box at landing height), wall_x_junction (column at intersection). IBC-compliant stair geometry: 17.8cm rise, 27.9cm run. | Stair has correct rise/run. Floor cutout matches stairwell dimensions. | 8h |
| B4.5 | Three-tier quality system | Same files | Tier 1 "Blockout": Box-only geometry, single material, <1ms per piece. Tier 2 "Functional": Box + openings + basic UV, 2-3 materials, <5ms. Tier 3 "Style-Matched": Detect bevel width from kit (dihedral angle analysis on existing wall pieces), apply matching bevels to fallbacks, attempt baseboard via bottom-face height segmentation, <20ms. Default: Tier 2. User override via `generation_quality` param. | Tier 1 fallback is a box. Tier 2 has openings. Tier 3 has matching bevel width (tested against a kit with 5cm beveled edges). | 5h |
| B4.6 | Fallback caching + invalidation | Same files | Cache generated fallbacks at `Saved/Monolith/ModularKits/{kit}/Fallbacks/`. Regenerate only when kit_params change (hash kit_params, compare on next build). `regenerate_fallbacks({kit})` action for manual refresh. | Generate fallbacks for a kit. Re-build -- no regeneration (cache hit). Change grid size -- fallbacks regenerate. | 3h |

**Done criteria for Phase B4:** All 14 fallback piece types generate correctly at matching kit dimensions. Three quality tiers work. Caching prevents redundant generation.

### Phase B5: Build With Kit + Proxy Swap (20-30h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| B5.1 | `build_with_kit` core action | `Private/MonolithPCGBuildWithKitActions.h/.cpp` (or extend kit scanner actions) | Input: kit_name + building_descriptor JSON. Process: Read kit JSON -> for each wall edge in building descriptor, map wall_type + opening_type to kit piece_type -> select best matching piece from kit (exact match, then closest dimensions, then fallback) -> place via HISM. One HISM component per unique mesh (wall_solid, wall_door, etc.), hosted on a single parent actor per building. Transform = edge position + rotation from building descriptor. Floors and ceilings via floor_tile HISM. SetFolderPath("Monolith/Buildings/{building_id}"). | Build a 6-room building using a scanned kit. All walls correct type. HISM draw calls < 10 per building. | 12h |
| B5.2 | Piece selection algorithm | Same files | For each edge, rank kit pieces by: 1) exact type match (wall_solid, wall_door, etc.), 2) dimension match (within 5cm tolerance), 3) variant diversity (avoid same variant repeatedly -- weighted random from available variants). If no kit piece matches: use fallback. If multiple match: prefer user_override > high_confidence > first_alphabetical. | Given a kit with 3 wall_solid variants, all 3 appear in a building (not just variant_01 repeated). | 5h |
| B5.3 | `swap_proxies` action | Same files | Replace blockout/whitebox geometry with real kit pieces. Input: actors to swap + kit_name. For each actor: get bounds -> find closest kit piece by dimensions and type tag -> replace actor with HISM instance of kit piece at same transform. Supports "swap all in folder" mode. | Replace 20 whitebox wall actors with kit pieces. All positions preserved. Original actors deleted. | 6h |
| B5.4 | `build_block_with_kit` action | Same files | Higher-level action: generate N buildings on a city block layout using a specific kit. Calls existing `create_city_block` -> building descriptors -> `build_with_kit` per building. Returns summary (building count, piece count, fallback count, draw calls). | Generate a 4-building block with a scanned kit in a single MCP call. | 5h |

**Done criteria for Phase B5:** End-to-end workflow works: scan kit -> build buildings -> see HISM instances in the level. Proxy swap replaces whitebox with real pieces.

### Phase B Summary

| Phase | Actions | Est Hours | Dependencies |
|-------|---------|-----------|--------------|
| B1: Asset Scan + Name Classification | scan_modular_kit (partial) | 20-25h | None |
| B2: Grid + Opening + Pivot | classify_mesh + grid/opening detection | 25-35h | B1 |
| B3: Kit Management | edit/list/export/import kit actions (~6) | 15-20h | B1, B2 |
| B4: Fallback Generation | regenerate_fallbacks + internal generators | 25-35h | B1, B2 |
| B5: Build With Kit | build_with_kit, swap_proxies, build_block_with_kit | 20-30h | B1-B4, A1 (for PCG placement), existing building system |
| **Total** | **~15-20 actions** | **120-160h** | |

---

## Sub-Plan C: PCG Town Dressing

**Estimated effort:** 200-280h
**Research docs:** `pcg-vegetation-research`, `pcg-horror-atmosphere-research`, `pcg-lighting-audio-research`, `pcg-vehicles-props-research`, `pcg-roads-research`
**Deliverable:** PCG graph templates + MCP actions for vegetation, horror atmosphere, lighting, props, and road networks.
**Dependency:** Requires Sub-Plan A Phase A1-A2 complete (graph construction + execution).

### Phase C1: Vegetation + Foliage (40-55h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| C1.1 | Base vegetation scatter graph template | `Private/MonolithPCGVegetationTemplates.cpp` | `create_vegetation_graph` builds: Landscape Data -> Surface Sampler (density param) -> Normal to Density (slope filter) -> Density Filter (min/max slope) -> Difference (building footprint exclusion) -> Self Pruning -> Static Mesh Spawner (weighted mesh list). User params: density, slope_min, slope_max, seed, mesh_list, exclusion_actors. | Grass spawns on flat areas, excluded from building footprints. | 10h |
| C1.2 | Tree + bush placement template | Same files | Separate template for larger vegetation: lower density, larger bounds for Self Pruning, distance-from-building filter via Distance node. Height-based species selection (taller trees in parks, smaller bushes near buildings). | Trees appear in open areas, bushes near building perimeters, no overlap. | 8h |
| C1.3 | Decay-driven overgrowth | Same files | Overgrowth increases with building decay param. PCG graph: sample points near building perimeters -> density = decay * max_density -> spawn overgrowth meshes (dead vines, weeds breaking through pavement, grass in gutters). Two modes: floor-level (weeds) and wall-level (wall decals for ivy/moss). | Decay 0.8 building has dense weeds at base, decay 0.2 building has minimal. | 10h |
| C1.4 | Ground cover (litter, mud, debris) | Same files | Surface scatter with decay-driven density. Different mesh tables per context: street (litter, paper, cans), yard (leaves, twigs), parking lot (oil stains decals). | Streets have litter, yards have leaves, density scales with decay. | 8h |
| C1.5 | Fog zone placement | Same files | Spawn Local Volumetric Fog actors via PCG SpawnActor node at low-lying areas. Place near water features, in alleys, in dense vegetation. Density driven by time-of-day param and horror_intensity. | Fog actors appear in alleys and low areas, with configurable density. | 6h |

### Phase C2: Horror Atmosphere -- 7-Layer System (50-65h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| C2.1 | Layer 1: Structural decay | `Private/MonolithPCGHorrorTemplates.cpp` | PCG graph driven by per-room decay attribute. Decay 0.5-0.8: spawn broken furniture variants, displaced ceiling tiles, cracked floor decals. Decay 0.8-1.0: collapsed wall sections, rubble piles, exposed rebar. Uses Attribute Filter on decay ranges. | Room at decay 0.7 has broken furniture. Room at 0.9 has rubble. Room at 0.2 is clean. | 10h |
| C2.2 | Layer 2: Surface contamination | Same files | Decal scatter on floors (blood, water stains, oil), walls (handprints, scratches, mold). Uses existing `place_decals` infrastructure but driven by PCG point generation for batch efficiency. Decal density = f(decay, room_type). | Bathroom has water stains, hallway has drag marks at high decay. | 8h |
| C2.3 | Layer 3: Debris + clutter | Same files | Room interior scatter: papers, broken glass, toppled chairs, scattered medical supplies. Mesh selection weighted by room_type (medical rooms get syringes, offices get papers). Uses PCG Polygon2D interior surface sampling on room boundaries. | Each room type has thematically appropriate debris. | 8h |
| C2.4 | Layer 4: Organic growth | Same files | Corner cobwebs (sample polygon vertices, spawn at corners where decay > 0.3). Mold patches (wall surface scatter, UV-aligned decals). Dead insects (floor scatter in high-decay rooms). | Cobwebs appear in corners of abandoned rooms. | 6h |
| C2.5 | Layer 5: Light degradation | Same files | Spawn light BP actors at ceiling height. Working/flickering/broken ratio driven by decay (70-80% broken at high decay). Flickering lights use Timeline BP with randomized keyframes. Broken lights are mesh-only (dark). | Horror corridor: mostly dark with 1-2 flickering lights and 1 working. | 8h |
| C2.6 | Layer 6: Audio atmosphere | Same files | Spawn ambient sound emitters via PCG SpawnActor: dripping water (bathrooms, high decay), electrical hum (near transformers), wind through broken windows, distant creaking. Volume = f(decay). | Each room type has appropriate ambient audio at correct volume. | 6h |
| C2.7 | Layer 7: Environmental storytelling | Same files | Composite "vignette" spawning: combine existing `place_storytelling_scene` patterns with PCG-driven placement. Select vignette type based on room_type + decay + tension_level. Place 0-2 vignettes per room. | Rooms have contextually appropriate horror vignettes. | 7h |

### Phase C3: Vehicles, Props, Street Furniture (35-50h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| C3.1 | Per-lot prop system | `Private/MonolithPCGPropTemplates.cpp` | PCG graph template: for each building lot, spawn context-appropriate props. Residential: mailbox, trash cans, garden hose, patio furniture. Commercial: dumpster, signage, AC units. Per-lot props placed at lot boundary from spatial registry. | Residential lot has mailbox at road edge, commercial has dumpster in back. | 10h |
| C3.2 | Street furniture | Same files | Spline-based placement: fire hydrants (90-180m, prefer corners), benches (20-50m near commercial), newspaper boxes (clusters near commercial), phone booths (rare), street signs (at intersections). All driven by road spline + building type data. | Street has regularly spaced hydrants, benches near shops, signs at intersections. | 10h |
| C3.3 | Vehicle placement | Same files | Driveway vehicles (0-2 per residential lot, probability = 1 - lot_abandonment). Street parking (parallel, along curb, probability-based per block). Abandoned vehicles: shifted/rotated from proper parking, doors open, broken windows. Wrecked vehicles at key horror locations. | Residential driveways have cars. Some abandoned cars block streets at high decay. | 8h |
| C3.4 | Horror prop overlays | Same files | Missing person posters (on poles, walls), police tape (across doors), quarantine signs, boarded windows (facade-aligned). Density = horror_intensity parameter. | High-horror areas have missing person posters and police tape. | 7h |

### Phase C4: Road Network Generation (40-55h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| C4.1 | Road graph to spline conversion | `Private/MonolithPCGRoadActions.h/.cpp` | Action: `create_road_network`. Input: road graph (nodes = intersections, edges = road segments with type). Output: USplineComponent per road segment. Support simple grid generator with randomized dead-ends for fully procedural mode. Also accept manually placed spline actors. | Generate a 4x3 block grid with 2 dead ends. Splines follow road centerlines. | 10h |
| C4.2 | Road surface + sidewalk | Same files | Spline mesh deformation for road surface. Sidewalk as offset spline with modular curb pieces. Road width from hierarchy (main: 1200cm, residential: 800cm, alley: 400cm). Curb modular pieces placed along spline via PCG Spline Sampler. | Roads have surfaces, curbs, and sidewalks at correct widths. | 12h |
| C4.3 | Road markings + infrastructure | Same files | Decal-based road markings (center line, parking lines). Street lamps along road splines (distance-based placement, working/broken ratio from decay). Power lines (catenary spline meshes between poles). | Roads have lane markings, regularly spaced street lamps, power lines. | 10h |
| C4.4 | Intersection handling | Same files | Dedicated intersection meshes/decals at spline junctions. T-junction and 4-way variants. Detect intersection points from road graph. Place intersection assets, extend/trim road splines to meet cleanly. | T-junctions and 4-way intersections have clean geometry and markings. | 10h |

### Phase C5: Integration + Presets (15-20h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| C5.1 | `dress_town_block` composite action | `Private/MonolithPCGTownDressingActions.cpp` | Orchestrator action: given a city block with buildings, run all dressing passes in order: roads -> vegetation -> street furniture -> per-lot props -> vehicles -> horror atmosphere. Each pass is a PCG graph template. Configurable via preset JSON. | Single call produces a fully dressed town block. | 8h |
| C5.2 | Dressing presets | Same files + `Saved/Monolith/PCGPresets/` | Pre-built presets: "abandoned_suburb", "active_downtown", "hospital_campus", "industrial_district". Each preset configures decay ranges, prop densities, vegetation types, road hierarchy, lighting ratios. | Apply "abandoned_suburb" preset, get appropriate dressing. Switch to "active_downtown", result changes accordingly. | 6h |

### Phase C Summary

| Phase | Actions | Est Hours |
|-------|---------|-----------|
| C1: Vegetation | ~5 templates | 40-55h |
| C2: Horror Atmosphere | 7-layer templates | 50-65h |
| C3: Props/Vehicles | ~4 templates | 35-50h |
| C4: Roads | ~4 actions | 40-55h |
| C5: Integration | 2 actions + presets | 15-20h |
| **Total** | **~25-30 actions/templates** | **200-280h** |

---

## Sub-Plan D: PCG Gameplay Integration

**Estimated effort:** 130-180h
**Research docs:** `pcg-gameplay-elements-research`
**Deliverable:** PCG-driven placement of gameplay elements (items, enemies, objectives, safe rooms).
**Dependency:** Requires Sub-Plan A (Phase A1-A2), Sub-Plan C (horror atmosphere for decay integration), existing spatial registry.

### Phase D1: Item + Loot Placement (30-40h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| D1.1 | Room-type loot tables (DataAssets) | `Private/MonolithPCGGameplayActions.h/.cpp` + DataAssets | Create `UMonolithLootTable` DataAsset class with room_type, weighted entries (item, weight, max_per_room), total_items range, empty_probability. Ship default tables: bathroom (health items), kitchen (food, melee), office (notes, keys), bedroom (health, notes), storage (ammo, tools), medical (health kits), security (ammo, weapons). Action: `create_loot_table`, `edit_loot_table`. | DataAssets load correctly. Weighted selection produces expected distribution over 100 samples. | 10h |
| D1.2 | PCG loot scatter template | Same files | Template graph: Room polygon -> Interior Surface Sampler -> Density Filter (by loot_density param) -> Self Pruning (min 50cm distance) -> Spawn Actor (BP_LootSpawnPoint with loot_table reference). BP spawn points are dormant until activated by AI Director. | Spawn points appear inside room bounds, correctly spaced, with room-type loot table assigned. | 10h |
| D1.3 | Surface-aware item placement | Same files | Enhanced placement: items on surfaces (shelves, tables, desks) not just floors. Use existing `scatter_on_surface` logic to find valid surfaces within rooms, then PCG scatter on those surfaces. Combine PCG room selection with Monolith surface detection. | Items appear on table tops and shelves, not floating in air. | 10h |
| D1.4 | Hospice accessibility | Same files | Configurable floor_min_health parameter guarantees minimum health items per floor. Items must have clear visual indicators (subtle glow material). Never place critical items in hard-to-reach locations (filter out high-Z surfaces, surfaces behind physics objects). | With floor_min_health=3, every floor has at least 3 health items accessible from floor level. | 5h |

### Phase D2: Enemy Spawn + AI Director Integration (35-50h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| D2.1 | Spawn point generation | Same files | PCG template: identify valid enemy spawn locations via spatial analysis. Criteria: not in player sightline from room entry (use sightline analysis from existing MonolithMesh), minimum distance from safe rooms, prefer rooms with single entry (ambush potential), prefer dark areas. Output: scored spawn points with type tags (lurker, patrol, ambush, horde). | Spawn points favor dark corners, avoid safe rooms, prefer single-entry rooms. | 12h |
| D2.2 | Patrol path generation | Same files | Generate patrol paths as splines through connected rooms. Use navmesh (existing) to find valid paths. PCG graph: room center points -> pathfinding subgraph -> output patrol splines. Patrol types: loop (returns to start), linear (back and forth), roaming (visits all rooms in zone). | Patrol splines follow valid navmesh paths through room doors. | 10h |
| D2.3 | Difficulty scaling | Same files | PCG parameter `difficulty` (0.0-1.0) controls: spawn point density, spawn type distribution (more lurkers at low, more horde at high), patrol path complexity, resource scarcity. Hospice mode caps difficulty at 0.3 and guarantees generous resources. | Difficulty 0.2: few enemies, generous items. Difficulty 0.8: many enemies, scarce items. | 8h |
| D2.4 | AI Director hooks | Same files | Expose PCG generation as AI Director input. Director can: activate/deactivate spawn points based on player performance, modify loot table weights reactively, trigger PCG regeneration for specific rooms. Actions: `set_room_threat_level`, `activate_spawn_zone`, `deactivate_spawn_zone`. | AI Director call to `set_room_threat_level({room: "kitchen", level: 0.8})` increases spawn density in that room. | 10h |

### Phase D3: Objectives + Progression (30-40h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| D3.1 | Lock-and-key placement | Same files | Graph-based progression: divide building into zones separated by locked doors. Place keys in preceding zones (never in the locked zone). Algorithm: build zone graph from spatial registry, identify chokepoint doors, assign lock types (key, keycard, puzzle), place keys in rooms reachable before the lock. Validate all zones are reachable. | 3-zone building: key_A in zone 1 unlocks door to zone 2, key_B in zone 2 unlocks door to zone 3. All zones reachable. | 12h |
| D3.2 | Safe room placement | Same files | Identify candidate safe rooms: single entry, interior room (no windows), moderate size. Score using existing `evaluate_safe_room` action. Place safe room markers (save point, storage chest, guaranteed health). PCG graph places safe room furnishing. | Safe rooms are defensible, have save points and health, appear every 3-5 rooms of progression. | 8h |
| D3.3 | Jump scare placement | Same files | Use existing `suggest_scare_positions` analysis. PCG spawns trigger volumes at identified scare positions. Scare types: proximity trigger, door-open trigger, item-pickup trigger, line-of-sight trigger. Intensity validated via `validate_horror_intensity` for hospice mode. | Scare triggers at good positions (dark corners, after tight turns, near objectives). Hospice mode has reduced intensity. | 8h |
| D3.4 | `dress_gameplay` composite action | Same files | Orchestrator: given a dressed town block, place all gameplay elements. Order: safe rooms -> lock/key -> loot -> spawn points -> patrol paths -> scare triggers. Validates all progression paths. | Single call produces a playable level with items, enemies, objectives, and valid progression. | 6h |

### Phase D Summary

| Phase | Actions | Est Hours |
|-------|---------|-----------|
| D1: Items/Loot | ~4 actions | 30-40h |
| D2: Enemies/AI | ~4 actions | 35-50h |
| D3: Objectives | ~4 actions | 30-40h |
| **Total** | **~12-15 actions** | **130-180h** |

---

## Sub-Plan E: Terrain + Landscape

**Estimated effort:** 95-130h
**Research docs:** `pcg-terrain-research`
**Deliverable:** Landscape modification actions for building foundations, road cuts, and ground texturing.
**Dependency:** Independent of PCG module (uses landscape APIs directly). PCG reads terrain after modification.

**Critical constraint:** PCG CANNOT write to landscape heightmaps or weightmaps. Terrain modification uses `FLandscapeEditDataInterface` directly.

### Phase E1: Landscape Modification (40-55h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| E1.1 | Flatten terrain for buildings | `Private/MonolithPCGTerrainActions.h/.cpp` (or MonolithMesh extension) | Action: `flatten_terrain_for_building({building_id})`. Read building footprint polygon from building descriptor. Use `FLandscapeEditDataInterface::GetHeightData()` to sample current heights within footprint + margin. Compute target height (average of perimeter samples). `SetHeightData()` to flatten footprint to target height. Apply Gaussian blur at edges for smooth blending. | Building footprint area is flat. Edges blend smoothly into surrounding terrain (no cliff). | 14h |
| E1.2 | Cut terrain for roads | Same files | Action: `cut_terrain_for_road({road_spline})`. Sample road spline points. For each point, flatten terrain within road width + shoulder. Maintain grade along road length (interpolate heights along spline). Cut/fill logic: where terrain is above road grade, cut; where below, fill. | Road runs at consistent grade through hilly terrain. | 14h |
| E1.3 | Retaining walls | Same files | Where cut depth > threshold (150cm), spawn retaining wall meshes along cut edge. Use spline-sampled placement. Material matches local terrain type. | Tall cuts have retaining walls, shallow cuts blend naturally. | 8h |
| E1.4 | Foundation generation | Same files | For buildings on slopes: detect max height differential across footprint. If > threshold: generate terrain-adaptive foundation (piers, stepped, cut-fill). Use GeometryScript to create foundation mesh adapted to terrain profile. | Building on a slope has a visible foundation raising the low side to level. | 10h |

### Phase E2: Ground Texturing (25-35h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| E2.1 | PCG-driven layer painting | Same files | After terrain modification, paint landscape layers: road surface layer along road splines, building foundation layer under buildings, grass layer in yards, dirt layer in neglected areas. Use `FLandscapeEditDataInterface::SetAlphaData()`. Decay parameter drives layer selection (well-maintained grass vs dead grass vs bare dirt). | Road areas painted with asphalt layer, yards with grass, high-decay areas with dead grass. | 12h |
| E2.2 | Drainage and low-area detection | Same files | Detect local minima in terrain (potential puddle/drainage locations). Spawn water puddle decals or shallow water planes at low points. In road context, detect and mark drainage gutter positions along curbs. | Low spots have puddles, road edges have drainage visual. | 8h |
| E2.3 | Ground detail scatter | Same files | PCG template for ground-level detail driven by landscape layer: gravel on dirt, leaves on grass, trash on asphalt. Uses landscape layer weight attribute from PCG Surface Sampler. | Gravel appears on dirt paths, leaves on grass, litter on asphalt. | 8h |

### Phase E Summary

| Phase | Actions | Est Hours |
|-------|---------|-----------|
| E1: Landscape Modification | ~4 actions | 40-55h |
| E2: Ground Texturing | ~3 actions | 25-35h |
| **Total** | **~7 actions** | **95-130h** |

---

## Sub-Plan F: Runtime + Streaming

**Estimated effort:** 155-225h
**Research docs:** `pcg-runtime-streaming-research`
**Deliverable:** Runtime PCG generation with World Partition integration, HiGen multi-scale, and on-demand interior generation.
**Dependency:** Requires Sub-Plans A, B, C substantially complete. This is the optimization/scale-up phase.

### Phase F1: HiGen + World Partition (50-70h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| F1.1 | HiGen grid configuration | `Private/MonolithPCGRuntimeActions.h/.cpp` | Configure PCG graphs for hierarchical generation. Map generation layers to HiGen grid levels: Grid2048 = town layout, Grid128 = block layout, Grid32 = building placement, Grid8 = room detail, Grid4 = micro-detail (debris, cobwebs). Action: `configure_higen({graph, grid_mapping})`. | Graph executes at correct grid levels. Block-level content generates before building-level. | 12h |
| F1.2 | Runtime generation trigger setup | Same files | Configure `EPCGComponentGenerationTrigger::GenerateAtRuntime` on PCG components. Set generation radii per grid level. Configure scheduler CVars: `pcg.RuntimeGeneration.GridSizeDefault`, `pcg.RuntimeGenScheduler.FrameBudgetMs` (5ms default). Action: `configure_runtime_generation({radius_map, budget_ms})`. | Content generates as player approaches, cleans up when player leaves. Stays within 5ms frame budget. | 12h |
| F1.3 | World Partition integration | Same files | Ensure PCG-generated actors participate in World Partition streaming. PCG partition actors auto-register. Configure streaming distances per content type (buildings visible far, interior detail only when close). Handle the known `GenerateOnDemand` partition actor bug (use `GenerateAtRuntime` instead). | Town content streams in/out with World Partition. No pop-in at boundaries (hysteresis multiplier). | 12h |
| F1.4 | Performance monitoring | Same files | Action: `get_pcg_runtime_stats`. Report: active PCG components, generation queue depth, frame budget usage, instance counts per HISM. Use `UPCGSubsystem` metrics. | Stats show generation load, budget utilization, and instance counts in real-time. | 8h |

### Phase F2: Runtime Interior Generation (45-65h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| F2.1 | On-demand interior generation | Same files | Buildings render as exterior-only shells at distance. When player approaches building entrance (proximity trigger), generate interior: walls, floors, furniture, dressing. Use `GenerateOnDemand` triggered by overlap volume at doorway. Pre-compute building descriptors at editor time, defer interior PCG execution to runtime. | Approach building -> interior generates in <500ms. Enter building -> fully furnished. | 15h |
| F2.2 | Interior LOD system | Same files | Three interior LODs: L0 (full detail: furniture, debris, decals, lights), L1 (structure only: walls, floors, doors), L2 (invisible: interior cleaned up). Distance-driven transitions. Furniture HISM instances removed/added per LOD. | Inside building: full detail. Adjacent building (through window): walls only. Far buildings: no interior. | 12h |
| F2.3 | Deterministic generation | Same files | Same seed = same building interior. Store building seeds in building descriptor. PCG seed propagation: building seed -> per-room seeds -> per-scatter seeds. Verify: generate building, cleanup, regenerate = identical placement. Critical for multiplayer and save/load. | Generate twice with same seed, diff the spawned actor positions -- all match. | 10h |
| F2.4 | Memory budget system | Same files | Track total instance counts, mesh memory, generated actor count. Configurable budget caps. When approaching budget: reduce detail level, increase cleanup aggressiveness, defer lower-priority generation. Action: `set_memory_budget({max_instances, max_generated_actors, max_mesh_memory_mb})`. | Stay under budget (e.g., 50K instances, 500 generated actors, 2GB mesh memory) even with 20+ generated buildings. | 10h |

### Phase F3: Scale Testing + Optimization (30-45h)

| WP | Task | Files | Algorithm/Approach | Test Criteria | Est |
|----|------|-------|--------------------|---------------|-----|
| F3.1 | GPU compute integration | Same files | Enable FastGeo for scatter-heavy graphs. CVar: `pcg.RuntimeGeneration.ISM.ComponentlessPrimitives=1`. Enable `PCG FastGeo Interop` plugin. Benchmark CPU vs GPU path for vegetation and debris scatter. | GPU scatter is measurably faster than CPU for 10K+ point operations. | 8h |
| F3.2 | Large-scale stress test | Test maps + profiling | Test with 100+ buildings, 5000+ HISM instances per block, 20+ blocks. Profile with Unreal Insights. Identify bottlenecks: generation time, draw calls, memory, streaming transitions. | 100-building town runs at 30+ FPS on target hardware with streaming. | 12h |
| F3.3 | Partition actor pooling | Same files | Enable PCG partition actor pooling to reduce spawn/destroy churn during streaming. Configure pool sizes per content type. | Streaming transitions don't cause frame hitches (pooled actors reused). | 8h |

### Phase F Summary

| Phase | Actions | Est Hours |
|-------|---------|-----------|
| F1: HiGen + World Partition | ~4 actions | 50-70h |
| F2: Runtime Interiors | ~4 actions | 45-65h |
| F3: Scale + Optimization | ~3 actions | 30-45h |
| **Total** | **~11 actions** | **155-225h** |

---

## Cross-Cutting Concerns

### Hospice Mode

Every sub-plan must respect hospice accessibility:
- **Items:** Generous minimums, clear visibility, accessible locations (D1.4)
- **Horror:** Intensity caps via `validate_horror_intensity` (C2.7, D3.3)
- **Difficulty:** Hospice mode caps at 0.3, generous resources (D2.3)
- **Controls:** All PCG parameters have hospice-safe defaults documented
- **Reports:** `generate_hospice_report` action validates entire generated level

### Outliner Organization

ALL spawned actors MUST use `SetFolderPath()`:
- `Monolith/PCG/Graphs/` -- PCG volumes and components
- `Monolith/PCG/Vegetation/` -- foliage scatter
- `Monolith/PCG/Props/` -- street furniture, vehicles
- `Monolith/PCG/Horror/` -- horror dressing
- `Monolith/Buildings/{building_id}/` -- building HISM
- `Monolith/Roads/` -- road infrastructure

### Testing Strategy

Each sub-plan phase requires:
1. **Unit tests:** Individual action input/output validation
2. **Integration tests:** Multi-action workflows (scan -> build -> dress -> gameplay)
3. **Visual verification:** Scene captures comparing generated results against reference
4. **Performance benchmarks:** Frame time, generation time, memory, draw calls

### Documentation Updates Required

After EACH phase completion:
- `Docs/SPEC.md` -- new actions documented
- `Docs/TODO.md` -- completed items marked
- `Docs/TESTING.md` -- test results recorded
- `CLAUDE.md` -- action count updated
- `.claude/skills/unreal-pcg.md` -- new skill file for PCG namespace (created at A1)
- Agent definitions updated if new agents needed

---

## File Conflict Analysis

### Within Sub-Plan A (MonolithPCG module)

No conflicts -- all new files in `Source/MonolithPCG/`.

### Between Sub-Plan A and B

**Decision: Kit Scanner lives in MonolithPCG, not MonolithMesh.**

Rationale: The kit scanner is deeply tied to PCG (scanned kits feed into PCG graph execution). While scanning uses Asset Registry and GeometryScript (MonolithMesh dependencies), the output drives PCG-based building assembly. Housing it in MonolithPCG keeps the feature self-contained.

MonolithPCG.Build.cs must depend on GeometryScript (for fallback generation) and GeometryCore (for FMeshBoundaryLoops). Add these as optional dependencies with `WITH_GEOMETRYSCRIPT` guard, same pattern as MonolithMesh.

### Between Sub-Plan B and existing MonolithMesh

B5.1 (`build_with_kit`) will use existing MonolithMesh infrastructure:
- `SaveMeshToAsset()` for fallback pieces
- `ConvertToHism()` for HISM assembly
- Spatial registry (MonolithMesh owns this)

Solution: MonolithPCG depends on MonolithMesh (private dependency). Kit scanner calls MonolithMesh functions internally.

### Between Sub-Plan C and existing MonolithMesh

Existing MonolithMesh has horror actions (decals, storytelling scenes, context props). PCG horror templates (C2) will call these internally for non-batch operations, but use PCG for batch placement. No file conflicts -- new files in MonolithPCG.

### Between Sub-Plans E (Terrain) and existing code

Terrain modification could live in MonolithMesh (where terrain actions already exist: `MonolithMeshTerrainActions.h/.cpp`) or MonolithPCG. **Decision: Keep in MonolithMesh** since it uses landscape APIs, not PCG. The existing terrain actions file gets extended.

Files affected: `MonolithMeshTerrainActions.h/.cpp` (extend with flatten/cut actions).

### Cross-Sub-Plan File Conflicts

| File | Sub-Plans Touching | Resolution |
|------|-------------------|------------|
| `Monolith.uplugin` | A (add module + PCG dep) | Single edit in A1.3 |
| `MonolithMeshTerrainActions.h/.cpp` | E (extend) | E extends existing, no conflict with A-D |
| `Saved/Monolith/ModularKits/*.json` | B (read/write), B5+A4 (read) | B owns write, others read-only |
| `Saved/Monolith/SpatialRegistry/*.json` | A4 (read), D (read/write) | Existing format, additive writes only |
| `Saved/Monolith/PCGPresets/*.json` | A3 (create), C5 (create) | Different preset categories, no conflict |

---

## Risk Register

| # | Risk | Impact | Probability | Mitigation |
|---|------|--------|-------------|------------|
| R1 | PCG API changes in UE 5.8 | High | Medium | Minimize custom PCG nodes (only 2-3). Graph construction uses stable public API. WITH_PCG guard allows graceful degradation. |
| R2 | Opening detection false positives on cooked meshes | Medium | Low | Editor-time only (research confirms source mesh preserves topology). Document as editor-only feature. |
| R3 | ISM instance count degrades above 10K | Medium | Low | Our use case stays under 10K per component (50-200 per building). Monitor in F3.2 stress test. |
| R4 | PCGEx dependency for roads | Medium | Medium | PCGEx is MIT licensed but external. Make it optional -- roads work without it (manual spline placement), PCGEx enables auto-generation. |
| R5 | Fallback piece UVs don't match kit art style | Low | High | Expected -- fallbacks are functional, not artistic. Three quality tiers mitigate (Tier 3 attempts style matching). Users can replace fallbacks with real art. |
| R6 | Kit scanner confidence too low on unusual kits | Medium | Medium | 5-signal fusion + user correction loop. Corrections persist. Confidence thresholds adjustable per kit. |
| R7 | PCG GenerateOnDemand partition actor bug (UE 5.7) | High | Confirmed | Use `GenerateAtRuntime` instead of `GenerateOnDemand` for partitioned components. Documented in research. |
| R8 | GeometryScript not available (optional dependency) | Medium | Low | WITH_GEOMETRYSCRIPT guard. If absent: fallback generation disabled, opening detection disabled, basic scan-only mode still works. |
| R9 | Memory pressure from large-scale generation | High | Medium | Memory budget system in F2.4. Interior LOD in F2.2. Cleanup aggressiveness scales with pressure. |
| R10 | Landscape edit layer API changes (5.7 deprecations) | Medium | Low | Non-edit-layer landscapes deprecated in 5.7. Our implementation targets edit layers from the start. |

---

## Agent Assignment Recommendations

| Sub-Plan | Phase | Recommended Agent | Rationale |
|----------|-------|-------------------|-----------|
| A1-A4 | Module + all PCG actions | `unreal-editor-tools` (Opus) | New module creation, Build.cs, plugin integration, C++ PCG API |
| B1-B3 | Kit scanning + classification | `unreal-mesh-expert` (Opus) | Mesh inspection, Asset Registry, boundary loops, GeometryScript |
| B4 | Fallback generation | `unreal-mesh-expert` (Opus) | GeometryScript mesh generation (existing proc gen expertise) |
| B5 | Build with kit | `unreal-mesh-expert` (Opus) | HISM assembly, building descriptor integration |
| C1-C2 | Vegetation + horror | `unreal-niagara-expert` (Opus) or `unreal-mesh-expert` | PCG template construction, environmental scatter |
| C3 | Props/vehicles | `unreal-mesh-expert` (Opus) | Prop placement, spatial awareness |
| C4 | Roads | `unreal-mesh-expert` (Opus) | Spline-based, GeometryScript roads |
| D1-D3 | Gameplay | `unreal-ai-expert` (Opus) | AI Director, spawn logic, progression design |
| E1-E2 | Terrain | `unreal-mesh-expert` (Opus) | Landscape API, terrain modification |
| F1-F3 | Runtime | `cpp-performance-expert` (Opus) | Performance, streaming, memory budgets |

**Parallel execution in Milestone 1:**
- Sub-Plan A (Phase A1) runs first (module bootstrap)
- Sub-Plan B (Phase B1-B3) can start immediately (no PCG dependency for scanning)
- Sub-Plan A (Phase A2-A4) runs in parallel with B after A1 completes
- Sub-Plan B (Phase B4-B5) runs after B1-B3, needs A1 for PCG-based placement

---

## Appendix: Complete Action List (~110-135 new actions)

### pcg_query namespace (~37 actions from Sub-Plan A)

| # | Action | Phase |
|---|--------|-------|
| 1 | `create_pcg_graph` | A1 |
| 2 | `list_pcg_graphs` | A1 |
| 3 | `get_pcg_graph_info` | A1 |
| 4 | `add_pcg_node` | A1 |
| 5 | `remove_pcg_node` | A1 |
| 6 | `connect_pcg_nodes` | A1 |
| 7 | `disconnect_pcg_nodes` | A1 |
| 8 | `set_pcg_node_params` | A1 |
| 9 | `list_pcg_node_types` | A1 |
| 10 | `add_pcg_parameter` | A2 |
| 11 | `set_pcg_parameter` | A2 |
| 12 | `get_pcg_parameters` | A2 |
| 13 | `execute_pcg` | A2 |
| 14 | `cleanup_pcg` | A2 |
| 15 | `get_pcg_output` | A2 |
| 16 | `cancel_pcg_execution` | A2 |
| 17 | `spawn_pcg_volume` | A2 |
| 18 | `spawn_pcg_component` | A2 |
| 19 | `set_pcg_component_graph` | A2 |
| 20 | `list_pcg_components` | A2 |
| 21 | `refresh_pcg_component` | A2 |
| 22 | `get_pcg_generated_actors` | A2 |
| 23 | `set_pcg_generation_trigger` | A2 |
| 24 | `create_scatter_graph` | A3 |
| 25 | `create_spline_scatter_graph` | A3 |
| 26 | `create_vegetation_graph` | A3 |
| 27 | `create_horror_dressing_graph` | A3 |
| 28 | `create_debris_graph` | A3 |
| 29 | `apply_pcg_preset` | A3 |
| 30 | `save_pcg_preset` | A3 |
| 31 | `list_pcg_presets` | A3 |
| 32 | `spatial_registry_to_pcg` | A4 |
| 33 | `pcg_output_to_registry` | A4 |
| 34 | `building_descriptor_to_pcg` | A4 |
| 35 | `create_room_scatter_graph` | A4 |
| 36 | `batch_execute_pcg` | A4 |

### pcg_query namespace (Kit Scanner actions from Sub-Plan B, ~15-20)

| # | Action | Phase |
|---|--------|-------|
| 37 | `scan_modular_kit` | B1 |
| 38 | `classify_mesh` | B2 |
| 39 | `detect_grid_size` | B2 |
| 40 | `detect_mesh_openings` | B2 |
| 41 | `edit_kit_classification` | B3 |
| 42 | `accept_uncertain` | B3 |
| 43 | `reset_kit_classification` | B3 |
| 44 | `list_kits` | B3 |
| 45 | `get_kit_info` | B3 |
| 46 | `delete_kit` | B3 |
| 47 | `export_kit` | B3 |
| 48 | `import_kit` | B3 |
| 49 | `regenerate_fallbacks` | B4 |
| 50 | `build_with_kit` | B5 |
| 51 | `swap_proxies` | B5 |
| 52 | `build_block_with_kit` | B5 |

### Additional actions from Sub-Plans C-F (~50-70)

Town dressing templates, road actions, gameplay actions, terrain actions, runtime configuration -- see individual sub-plan phases for details.

**Grand total: ~110-135 new actions, taking Monolith from 684 to ~800-820 actions.**

---

## Research Document Index

| Doc | Key Finding | Used In |
|-----|-------------|---------|
| `pcg-framework-research` | PCG data type hierarchy, 100+ node types, FPCGPoint structure | A1, A2 |
| `pcg-mcp-integration-research` | Graph construction API (AddNodeOfType, AddEdge), 35-action design, Build.cs pattern | A1-A4 |
| `pcg-building-examples-research` | 76 examples, Cassini Shape Grammar, community tutorial patterns | A3, C4 |
| `hybrid-gs-pcg-research` | Three-layer architecture, Polygon2D for rooms, PCG vs GS decision matrix | A4, architecture |
| `pcg-vegetation-research` | Surface Sampler + slope filter + exclusion zones, decay-driven overgrowth, 60-85h | C1 |
| `pcg-roads-research` | PCGEx for graph-based roads, spline mesh deformation, intersection handling | C4 |
| `pcg-horror-atmosphere-research` | 7-layer decay system, existing Monolith overlap analysis, BP actor templates for lights | C2 |
| `pcg-gameplay-elements-research` | L4D AI Director pattern, room-type loot tables, hospice accessibility | D1-D3 |
| `pcg-terrain-research` | PCG is read-only for landscape, FLandscapeEditDataInterface for writes | E1-E2 |
| `pcg-lighting-audio-research` | BP actor templates for lights/audio, street light decay ratios, MegaLights | C2.5, C2.6 |
| `pcg-vehicles-props-research` | 28+ prop types, placement context hierarchy, per-lot vs street placement | C3 |
| `pcg-runtime-streaming-research` | HiGen grid levels, 5ms frame budget, partition actor pooling, GenerateOnDemand bug | F1-F3 |
| `asset-scanning-research` | 5-signal classification pipeline, IAssetRegistry API, UMonolithKitMetadata | B1 |
| `mesh-opening-detection-research` | FMeshBoundaryLoops, boundary loop -> opening classification, 95% accuracy | B2 |
| `marketplace-kit-conventions-research` | Naming conventions (SM_ prefix 90%), grid sizes (100-512cm), pivot conventions | B1, B2 |
| `kit-scanner-ux-research` | 3-turn golden path, confidence tiers, progressive disclosure, correction UX | B1, B3 |
| `grid-detection-research` | Candidate scoring + histogram validation + industry prior bias, >95% accuracy | B2 |
| `fallback-piece-generation-research` | 14 piece types, 3 quality tiers, dimension matching, Selection+Inset for openings | B4 |
| `modular-building-research` | AAA modular approach (Bethesda, Arkane, Embark), HISM instancing, wall-type mapping | B5 |
| `modular-pieces-research` | GeometryScript generation pipeline, AppendBox + Selection+Inset, existing SaveMeshToAsset | B4 |
| `hism-assembly-research` | HISM C++ API, AddInstances batch, PreAllocateInstancesMemory, spatial queries, ID bug on HISM | B5 |
