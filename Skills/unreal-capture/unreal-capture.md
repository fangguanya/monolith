---
name: unreal-capture
description: Use when capturing screenshots or previews of Unreal Engine assets and viewports via Monolith MCP — viewport capture, map management, static mesh preview, skeletal mesh preview, animation sequence frames, Niagara particle capture, material preview, widget blueprint capture, run artifact management. Triggers on capture, screenshot, preview, viewport, snapshot, render, thumbnail, artifact.
---

# Unreal Capture Workflows

You have access to **Monolith** with **13 capture actions** across 4 categories via the `capture` namespace.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "capture" })
```

## Key Parameter Names

- `asset_path` — 资产路径（StaticMesh / SkeletalMesh / AnimSequence / NiagaraSystem / Material / WidgetBlueprint）
- `camera` — 相机参数对象 `{location:[x,y,z], rotation:[p,y,r], fov:60}`
- `resolution` — 分辨率数组 `[width, height]`，默认 `[512,512]`
- `output_path` — 输出 PNG 文件路径
- `output_dir` — 多帧输出目录
- `run_id` — artifact 归档使用的 run-id
- `artifact_root` — 显式 artifact 输出目录
- `capability` — artifact 路径分类使用的 capability 名称
- `map_path` — 地图资产路径，例如 `/Game/Maps/DEMO1`
- `timestamps` — 时间采样点数组（秒），用于多帧序列截图
- `show_bones` — 是否显示骨骼叠加（SkeletalMesh / Animation）
- `preview_mesh` — Material 预览几何体：`plane`、`sphere`、`cube`

## Action Reference

### Map Management (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_current_map` | — | 获取当前打开地图的名称和路径 |
| `open_map` | map_path | 打开指定地图 |
| `save_map` | — | 保存当前打开的地图 |

### Viewport & Camera (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `capture_viewport` | capability?, run_id?, artifact_root?, camera_location?, camera_rotation? | 抓取当前主关卡编辑器视口截图 |
| `set_viewport_camera` | location?, rotation? | 设置编辑器主视口的相机位置和朝向 |
| `list_run_artifacts` | run_id | 列举指定 run-id 下的所有 artifact 文件 |

### Asset Preview Capture (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `capture_static_mesh` | asset_path, camera?, resolution?, output_path? | StaticMesh 离屏 3D 预览截图 |
| `capture_skeletal_mesh` | asset_path, show_bones?, camera?, resolution?, output_path? | SkeletalMesh 预览截图，支持骨骼显示 |
| `capture_animation` | asset_path, timestamps?, show_bones?, camera?, resolution?, output_dir?, filename_prefix? | AnimSequence 多帧采样截图 |
| `capture_niagara` | asset_path, seek_time?, camera?, resolution?, output_path? | Niagara 粒子系统预览截图 |
| `capture_material` | asset_path, preview_mesh?, camera?, resolution?, output_path? | Material 资产预览截图 |

### Widget & Sequence (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `capture_widget` | asset_path, resolution?, output_path? | WidgetBlueprint 设计器预览截图 |
| `capture_sequence_frames` | asset_path, asset_type, timestamps, show_bones?, camera?, resolution?, output_dir?, filename_prefix? | 对支持时间轴的资产进行多帧序列截图（niagara / animation） |

## Common Workflows

### 视口截图归档
```
# 1. 确认当前地图
capture_query({ action: "get_current_map" })

# 2. 设置相机角度
capture_query({ action: "set_viewport_camera", params: { location: {x:0, y:0, z:500}, rotation: {pitch:-45, yaw:0, roll:0} }})

# 3. 截取视口
capture_query({ action: "capture_viewport", params: { run_id: "my-run-001" }})

# 4. 查看 artifact 列表
capture_query({ action: "list_run_artifacts", params: { run_id: "my-run-001" }})
```

### 资产批量预览
```
# StaticMesh 预览
capture_query({ action: "capture_static_mesh", params: { asset_path: "/Game/Meshes/MyMesh", resolution: [1024, 1024] }})

# SkeletalMesh 带骨骼预览
capture_query({ action: "capture_skeletal_mesh", params: { asset_path: "/Game/Characters/SK_Char", show_bones: true }})

# Material 预览（使用球体）
capture_query({ action: "capture_material", params: { asset_path: "/Game/Materials/M_Wall", preview_mesh: "sphere" }})

# Niagara 粒子（推进 2 秒）
capture_query({ action: "capture_niagara", params: { asset_path: "/Game/FX/NS_Fire", seek_time: 2.0 }})
```

### 动画序列多帧采样
```
# 使用 capture_animation 直接采样
capture_query({ action: "capture_animation", params: {
  asset_path: "/Game/Animations/Idle",
  timestamps: [0.0, 0.5, 1.0, 1.5],
  show_bones: true,
  output_dir: "E:/CityGenerator/Saved/Screenshots/AnimFrames"
}})

# 或使用通用 capture_sequence_frames
capture_query({ action: "capture_sequence_frames", params: {
  asset_path: "/Game/FX/NS_Fire",
  asset_type: "niagara",
  timestamps: [0.0, 0.5, 1.0, 2.0, 3.0]
}})
```
