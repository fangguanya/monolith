---
name: unreal-light
description: Use when querying or modifying light components, sky lights, post-process volumes, sky atmosphere, height fog, volumetric clouds, IES profiles, reflection captures, and lightmap settings in this repository via Monolith MCP. Supports both Blueprint assets (asset_path) and scene actors (actor_name). Triggers on light, lighting, sky, atmosphere, fog, cloud, post-process, exposure, bloom, IES, reflection, lumen, lightmap.
---

# Unreal Light Workflows

You have access to **Monolith** with **25 light actions** across 8 categories via the `light` namespace.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "light" })
```

## Key Parameter Names

- `asset_path` — Blueprint 资产路径，用于操作 Blueprint CDO 上的组件（与 actor_name 二选一）
- `actor_name` — 场景 Actor 名称（当前关卡中的实例），用于操作运行时场景 Actor（与 asset_path 二选一）
- `component_name` — 指定特定光照组件名称（可选，默认取第一个匹配组件）
- `property_name` — 要设置的属性名称
- `value` — 新属性值（数值、布尔、颜色对象等）
- `ies_path` — IES 光照配置文件资产路径
- `path_filter` — 资产搜索目录过滤器
- `resolution` — 光照贴图分辨率（2 的幂）
- `quality` — 光照构建质量：Preview, Medium, High, Production

## Parameter Resolution

所有支持双参数的 action 遵循以下优先级：
1. **`actor_name`** 优先 — 在当前编辑器世界中搜索场景 Actor（精确标签 → 精确 FName → 子串匹配）
2. **`asset_path`** 备选 — 加载 Blueprint 并操作其 CDO（Class Default Object）
3. 两者都不提供时返回错误

## Action Reference

### Light Components (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_light_properties` | asset_path \| actor_name, component_name? | 获取光照组件全部属性（Intensity, Color, CastShadows, AttenuationRadius 等） |
| `set_light_property` | asset_path \| actor_name, property_name, value, component_name? | 设置光照组件属性（Intensity, Color, CastShadows, AttenuationRadius, InnerConeAngle, OuterConeAngle） |
| `list_light_components` | asset_path \| actor_name | 列出 Actor/Blueprint 上所有光照组件 |
| `add_light_component` | asset_path, light_type, component_name? | 向 Blueprint 添加光照组件（仅 Blueprint） |
| `remove_light_component` | asset_path, component_name | 从 Blueprint 移除光照组件（仅 Blueprint） |

### IES Profiles (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_light_profile` | asset_path \| actor_name, component_name? | 获取光照组件的 IES 配置文件 |
| `set_light_profile` | asset_path \| actor_name, ies_path, component_name? | 设置光照组件的 IES 配置文件 |
| `list_ies_profiles` | path_filter? | 列出项目中所有 IES 光照配置文件资产 |

### Post-Process Volume (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_post_process_settings` | asset_path \| actor_name | 获取后处理体积设置（bloom, exposure, vignette） |
| `set_post_process_property` | asset_path \| actor_name, property_name, value | 设置后处理属性（BloomIntensity, AutoExposureMinBrightness, AutoExposureMaxBrightness, VignetteIntensity） |

### Sky Light (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_sky_light_info` | asset_path \| actor_name | 获取天光组件配置（intensity, cast_shadows, volumetric_scattering_intensity） |
| `set_sky_light_property` | asset_path \| actor_name, property_name, value | 设置天光属性（Intensity, CastShadows, VolumetricScatteringIntensity） |
| `recapture_sky_light` | asset_path \| actor_name | 触发天光重新捕获 |

### Sky Atmosphere (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_sky_atmosphere_info` | asset_path \| actor_name | 获取天空大气组件设置 |
| `set_sky_atmosphere_property` | asset_path \| actor_name, property_name, value | 设置天空大气属性（AtmosphereHeight, MultiScatteringFactor） |

### Height Fog (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_height_fog_info` | asset_path \| actor_name | 获取指数级高度雾设置 |
| `set_height_fog_property` | asset_path \| actor_name, property_name, value | 设置高度雾属性（FogDensity, FogHeightFalloff, FogMaxOpacity, StartDistance, EnableVolumetricFog） |

### Volumetric Cloud (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_volumetric_cloud_info` | asset_path \| actor_name | 获取体积云组件设置 |
| `set_volumetric_cloud_property` | asset_path \| actor_name, property_name, value | 设置体积云属性（LayerBottomAltitude, LayerHeight） |

### Lightmap & Reflection (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_lightmap_settings` | — | 获取当前关卡光照贴图构建设置 |
| `set_lightmap_resolution` | asset_path, resolution, component_name? | 设置静态网格体的光照贴图分辨率 |
| `build_lighting` | quality? | 触发当前关卡的光照构建 |
| `list_reflection_captures` | — | 列出当前关卡所有反射捕获 Actor |
| `update_reflection_captures` | — | 更新当前关卡所有反射捕获 |

### Lumen (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_lumen_settings` | asset_path \| actor_name | 获取 Lumen GI 和反射设置 |
| `set_lumen_property` | asset_path \| actor_name, property_name, value | 设置 Lumen GI 或反射属性 |

## Pipeline Examples

### 查询场景中天光信息
```
light_query({ action: "get_sky_light_info", params: { actor_name: "SkyLight" } })
```

### 修改场景中方向光强度
```
light_query({ action: "set_light_property", params: { actor_name: "DirectionalLight", property_name: "Intensity", value: 5.0 } })
```

### 查询 Blueprint 资产中的光照组件
```
light_query({ action: "list_light_components", params: { asset_path: "/Game/Blueprints/BP_StreetLight" } })
```

### 修改场景后处理曝光
```
light_query({ action: "set_post_process_property", params: { actor_name: "PostProcessVolume", property_name: "AutoExposureMaxBrightness", value: 2.0 } })
```

### 触发天光重新捕获
```
light_query({ action: "recapture_sky_light", params: { actor_name: "SkyLight" } })
```

### 构建光照
```
light_query({ action: "build_lighting", params: { quality: "Preview" } })
```
