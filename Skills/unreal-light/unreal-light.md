---
name: unreal-light
description: Use when working with Unreal Engine lighting and atmosphere via Monolith MCP — light components, IES profiles, post-process volumes, sky light, sky atmosphere, Lumen GI, exponential height fog, volumetric clouds, lightmap building, and reflection captures. Triggers on light, lighting, IES, post-process, sky, atmosphere, Lumen, fog, volumetric cloud, reflection capture, lightmap.
---

# Unreal Light Workflows

You have access to **Monolith** with **27 light actions** across 7 categories via the `light` namespace.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "light" })
```

## Key Parameter Names

- `asset_path` — Blueprint or actor path containing light components
- `component_name` — name of the specific light component (optional — defaults to first found)
- `property_name` — the property to get/set
- `value` — new value (number, bool, color object, string)
- `light_type` — PointLight, SpotLight, RectLight, DirectionalLight, SkyLight
- `ies_path` — IES texture light profile asset path

## Action Reference

### Light Components (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_light_properties` | asset_path, component_name? | Get all light properties |
| `set_light_property` | asset_path, property_name, value, component_name? | Set a light property |
| `list_light_components` | asset_path | List all lights in BP |
| `add_light_component` | asset_path, light_type, component_name? | Add light to BP |
| `remove_light_component` | asset_path, component_name | Remove light from BP |

### IES Profiles (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_light_profile` | asset_path, component_name? | Get assigned IES profile |
| `set_light_profile` | asset_path, ies_path, component_name? | Assign IES profile |
| `list_ies_profiles` | path_filter? | List available IES assets |

### Post-Process (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_post_process_settings` | asset_path | Get PP volume settings |
| `set_post_process_property` | asset_path, property_name, value | Set PP property |

### Sky Light (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_sky_light_info` | asset_path | Get sky light config |
| `set_sky_light_property` | asset_path, property_name, value | Set sky light property |
| `recapture_sky_light` | asset_path | Trigger recapture |

### Lightmap & Reflection (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_lightmap_settings` | — | Get lightmap build settings |
| `set_lightmap_resolution` | asset_path, resolution, component_name? | Set lightmap resolution |
| `build_lighting` | quality? | Build lighting (Preview/Medium/High/Production) |
| `list_reflection_captures` | — | List reflection capture actors |
| `update_reflection_captures` | — | Update all reflection captures |

### Sky Atmosphere (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_sky_atmosphere_info` | asset_path | Get atmosphere settings |
| `set_sky_atmosphere_property` | asset_path, property_name, value | Set atmosphere property |

### Lumen (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_lumen_settings` | asset_path | Get Lumen GI/reflection settings |
| `set_lumen_property` | asset_path, property_name, value | Set Lumen property |

### Height Fog (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_height_fog_info` | asset_path | Get exponential fog settings |
| `set_height_fog_property` | asset_path, property_name, value | Set fog property |

### Volumetric Cloud (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_volumetric_cloud_info` | asset_path | Get cloud settings |
| `set_volumetric_cloud_property` | asset_path, property_name, value | Set cloud property |

## Common Workflows

### Set up outdoor lighting
1. `add_light_component` — add DirectionalLight as sun
2. `set_light_property` — set intensity, color, rotation
3. `set_sky_atmosphere_property` — configure atmosphere
4. `set_height_fog_property` — add atmospheric fog
5. `recapture_sky_light` — update sky light cubemap
6. `build_lighting` — bake (or rely on Lumen for dynamic)

### Lighting quality pass
1. `list_light_components` — audit all lights
2. `get_light_properties` — review each light
3. `set_light_property` — adjust intensity/color
4. `set_light_profile` — assign IES profiles
5. `get_post_process_settings` — check PP volume
6. `set_post_process_property` — tune bloom, exposure, color grading

### Supported set_light_property names
- `Intensity` — light intensity (float)
- `Color` — light color (object: {r, g, b, a?})
- `CastShadows` — shadow casting (bool)
- `AttenuationRadius` — point/spot light radius (float)
- `InnerConeAngle` — spot light inner cone (float)
- `OuterConeAngle` — spot light outer cone (float)
