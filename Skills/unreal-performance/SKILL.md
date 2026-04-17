---
name: unreal-performance
description: Use when analyzing or optimizing Unreal Engine performance via Monolith MCP — 138 perf.* runtime APIs (17 UE domains), .utrace deep analysis (10 analyzers), LLM memory tracking, config auditing, material shader stats, draw call analysis, INI tuning, PIE startup profiling. Triggers on performance, optimization, FPS, frame time, GPU, draw calls, shader complexity, profiling, memreport, trace, insights, stat, hitch, memory leak, LLM, DDC, streaming, PIE slow.
---

# Unreal Performance Analysis via Monolith MCP

**MonolithPerf** 模块提供 `perf` namespace 共 **138 个 MCP action**，覆盖 UE 5.7 全部 17 个性能分析域 + .utrace 深度分析。

> **PIE 启动卡顿专题**：如果问题涉及 PIE 卡/PIE 慢/冷启慢/FlushAsyncLoading，**必须先读** [`matrix-pie-startup-profiling` SKILL](../matrix-pie-startup-profiling/SKILL.md)，按其 SOP 执行。

## 工具总览

### 1. 运行时性能 API（136 个，perf_query）

| 域 | 代表 action | 数量 |
|---|---|---|
| **Trace 控制** | `start_trace` / `stop_trace` / `pause_trace` / `resume_trace` / `trace_status` / `trace_bookmark` / `trace_list_channels` / `trace_snapshot` | 8 |
| **Stat 系统** | `stat_list_groups` / `stat_dumpframe` / `stat_dumpave` / `stat_dumphitches` / `stat_dumpevents` / `stat_slow_configure` / `stat_unit_peak` / `stat_none` | 9 |
| **Stat 域封装** | `get_core_stats` / `get_memory_stats` / `get_shadow_stats` / `get_physics_stats` / `get_audio_stats` / `get_animation_stats` / `get_network_stats` / `get_task_stats` | 8 |
| **GPU** | `profile_gpu_frame` / `profile_gpu_with_drawcalls` / `capture_gpu_frames` / `get_gpu_realtime` / `get_gpu_memory` / `get_nanite_stats` / `get_lumen_stats` / `get_raytracing_stats` / `get_vsm_stats` / `get_substrate_stats` / `list_draw_calls` / `profile_gpu_set_sort` | 12 |
| **Shader/PSO** | `dump_shader_compile_stats` / `get_shader_compile_jobs` / `get_pso_cache_stats` / `get_pso_precache_stats` / `get_shader_memory` / `get_material_permutation_count` / `list_materials_by_cost` / `dump_material_shader_map` | 8 |
| **DDC/Zen** | `get_ddc_stats` / `zen_http_stats` / `zen_health` / `zen_list_namespaces` / `ddc_dump` / `ddc_backend_graph` | 6 |
| **Memory** | `capture_memreport` / `parse_memreport_section` / `diff_memreports` / `get_platform_memory` / `get_platform_memory_constants` / `dump_memory_by_class` / `dump_class_hierarchy` / `find_asset_refs` / `gc_collect` / `gc_dump_reachable` / `malloc_leak_snapshot` / `malloc_leak_check` | 12 |
| **LLM Tracker** | `llm_dump_context` / `llm_list_tags` / `llm_report` / `llm_tag_values` | 4 |
| **Streaming** | `list_streaming_textures` / `list_waiting_textures` / `get_streaming_pool_size` / `get_streaming_pool_status` / `dump_texture_streaming` / `force_stream_all_used` / `get_vt_residency` / `dump_vt_pool_usage` | 8 |
| **Level/WP/HLOD** | `list_streaming_levels` / `wp_cells_at_location` / `wp_list_loaded_cells` / `wp_list_streaming_sources` / `hlod_list_transitions` / `get_hlod_stats` / `get_world_partition_status` | 7 |
| **Niagara** | `niagara_dump_components` / `niagara_list_systems` / `niagara_emitter_optim_info` / `niagara_simcache_stats` / `niagara_per_system_cost` / `niagara_gpu_cpu_split` / `niagara_get_quality_level` / `niagara_debug_hud_toggle` | 8 |
| **Animation** | `abp_per_actor_cost` / `abp_dump_states` / `list_active_montages` / `get_anim_sharing_stats` | 4 |
| **Physics** | `get_chaos_stats` / `get_chaos_collisions` / `get_active_rigidbody_count` / `chaos_dump_evolution` | 4 |
| **Audio** | `get_active_voice_count` / `audio_dump_concurrency` / `audio_list_active_sounds` / `metasound_list_active` | 4 |
| **Network** | `get_net_bandwidth` / `net_rpc_by_class` / `net_list_connections` / `net_dump_replication_graph` | 4 |
| **Task/Thread** | `get_taskgraph_stats` / `get_thread_stats` / `get_async_loading_thread_state` / `get_parallelfor_stats` | 4 |
| **CSV Profiler** | `capture_csv_profile` / `csv_breadcrumb` / `csv_metadata` / `csv_list_categories` / `csv_set_category` / `summarize_csv` / `detect_csv_hitches` / `csv_list_metrics` | 8 |
| **Asset** | `get_asset_dependencies` / `get_asset_referencers` / `get_asset_registry_stats` | 3 |
| **Material** | `get_material_stats` / `get_material_complexity` | 2 |
| **通用** | `execute_console_command` / `run_commandlet` / `time_between` / `get_frame_stats` / `get_rendering_stats` / `get_streaming_stats` / `actor_count_by_class` / `get_pie_perf_snapshot` / `capture_stats_session` / `get_full_snapshot` / `stop_all_overlays` | 11 |

### 2. .utrace 深度分析（analyze_trace）

```
perf_query({ action: "analyze_trace", params: { trace_path: "E:/path/to.utrace", top_n: 15 } })
```

**10 个 IAnalyzer**，返回完整 JSON：

| 数据块 | 关键字段 |
|---|---|
| **CPU timing** | `top_timers_by_total[].{name, count, total_sec, max_ms, avg_ms}` + `top_hitches_by_max[]` |
| **Counters** | `counters[].{name, value}` — 228+ 含 LLM/DDC/RDG/Chaos/Scene/Shader/AsyncLoading |
| **Bookmarks** | `bookmarks[].{name, time_sec}` |
| **Diagnostics** | `diagnostics.{platform, app_name, branch, changelist, command_line}` |
| **Frames** | `game_frame_count`, `render_frame_count` |
| **Log** | `log.{total, errors, warnings}` |
| **LoadTime** | `async_package_count`, `top_packages_by_load_time[]` |
| **File I/O** | `file_io.{reads, writes, opens, bytes_read_mb, bytes_written_mb}` |
| **LLM Tags** | `llm_tags[].{tag, bytes, mb}` — 需 `-trace=memory` 启动 |
| **Allocations** | `memory_allocations.{alloc_count, free_count, peak_live_mb, leak_count}` — 需 `-trace=memalloc` 启动 |

**Trace channel 要求**：
- 基础：`default,cpu,frame,bookmark,counters`
- 完整：加 `log,loadtime,file,memory`
- 内存分配：`memalloc`（引擎启动时指定，read-only）
- LLM 运行时：引擎启动加 `-llm`

### 3. Config / Material / Niagara 审计

| 域 | 工具 | 用途 |
|---|---|---|
| Config | `config_query.resolve_setting` / `explain_setting` / `diff_from_default` / `search_config` | INI CVar 审计 |
| Material | `material_query.validate_material` / `get_all_expressions` | Shader 指令数/纹理采样 |
| Niagara | `niagara_query.list_emitters` / `list_renderers` / `get_compiled_gpu_hlsl` | VFX 复杂度 |

## 常用工作流

### 快速性能概览
```
perf_query({ action: "get_pie_perf_snapshot" })
```

### PIE 启动端到端分析
```
perf_query({ action: "start_trace", params: { channels: "default,cpu,frame,bookmark,counters,loadtime,log" } })
pie_query({ action: "start" })
# wait...
pie_query({ action: "stop" })
perf_query({ action: "stop_trace" })
perf_query({ action: "analyze_trace", params: { trace_path: "Saved/Profiling/xxx.utrace", top_n: 20 } })
```

### GPU 分析（PIE 中）
```
perf_query({ action: "profile_gpu_frame", params: { top_n: 20 } })
perf_query({ action: "get_nanite_stats" })
perf_query({ action: "get_lumen_stats" })
perf_query({ action: "get_gpu_memory" })
```

### 内存分析
```
perf_query({ action: "capture_memreport" })
perf_query({ action: "get_platform_memory" })
perf_query({ action: "dump_memory_by_class", params: { class_name: "Texture2D", top_n: 20 } })
perf_query({ action: "llm_tag_values", params: { top_n: 30 } })
```

### DDC / Shader
```
perf_query({ action: "get_ddc_stats" })
perf_query({ action: "zen_health" })
perf_query({ action: "dump_shader_compile_stats" })
```

### Hitch 检测
```
perf_query({ action: "stat_dumphitches" })
perf_query({ action: "detect_csv_hitches", params: { path: "Saved/Profiling/CSV/xxx.csv", threshold_ms: 50 } })
```

## High-Impact INI Settings

| Setting | Impact | Notes |
|---------|--------|-------|
| `r.Lumen.TraceMeshSDFs` | ~1-2ms GPU | 0 if not using mesh SDF |
| `r.Shadow.Virtual.SMRT.RayCountDirectional` | ~0.5ms GPU | 4 often sufficient |
| `gc.IncrementalBeginDestroyEnabled` | Frame spikes | Enable for GC hitches |
| `r.Streaming.PoolSize` | VRAM | Check via `get_streaming_pool_size` |
| `r.Lumen.Reflections.AsyncCompute` | White flash | Keep 0 until 5.7.2 |

## PIE 需求标记

GPU stat / Physics / Audio / Animation / Niagara 等标记 `pie_required` 的 API 在非 PIE 返回 `{ok:false, error:"perf.* requires active PIE"}`。

## 关联

| SKILL | 用途 |
|---|---|
| [`matrix-pie-startup-profiling`](../matrix-pie-startup-profiling/SKILL.md) | PIE 启动卡顿 SOP + C1-C5 元凶 |
| [`matrix-cold-start-bench`](../matrix-cold-start-bench/SKILL.md) | PIE 冷启自动化 bench |
| [`matrix-insights-trace`](../matrix-insights-trace/SKILL.md) | Trace capture + analyze |
