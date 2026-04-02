---
name: unreal-build
description: "Use when building, compiling, or fixing Unreal build errors in this repository. Contains the fixed Live Coding vs UBT pipeline and avoids discover-first Monolith usage."
---

# Unreal Build

用于本仓库的 UE 编译、Live Coding、UBT 构建与构建错误排查。

## 覆盖场景

- 日常编译
- Live Coding
- 结构性改动后的完整重编
- 编译失败后重启编辑器并恢复地图

## 固定 Pipeline

1. 先看改动类型。
2. 仅 `.cpp` 逻辑改动才优先考虑 Live Coding。
3. `.h`、`.Build.cs`、`.uplugin`、新增/删除源码文件，统一走完整 UBT。
4. 不要先 `monolith_discover()`；`editor` 的构建动作是已知面。

## 改动类型判定

| 改动 | 构建方式 |
|---|---|
| 仅 `.cpp` 函数体 | Live Coding |
| 修改 `.h` | UBT |
| 新增 `.cpp` / `.h` | UBT |
| 删除 `.cpp` / `.h` | UBT |
| 修改 `.Build.cs` | UBT |
| 修改 `.uplugin` | UBT |

规则：只要命中一条 UBT 条件，就整次构建都按 UBT 处理。

## Live Coding Pipeline

```text
1. editor_query({ action: "get_build_status", params: {} })
2. editor_query({ action: "trigger_build", params: {} })
3. 等待 10 秒，或轮询 get_build_status
4. editor_query({ action: "get_compile_output", params: {} })
5. 失败时再调 editor_query({ action: "get_build_errors", params: { compile_only: true } })
```

## UBT Pipeline

```text
1. 关闭编辑器
2. 运行项目标准 Build.bat / UBT 命令
3. 读取终端输出
4. 如需交叉验证，再看 get_compile_output / get_build_errors
```

本仓库路径约定（所有路径均相对于 repo 根目录 `<repo_root>`）：

| 变量 | 相对路径 |
|---|---|
| Engine Build.bat | `Engine/Build/BatchFiles/Build.bat` |
| UnrealEditor | `Engine/Binaries/Win64/UnrealEditor.exe` |
| 客户端项目 | `Client/CitySample.uproject` |
| Editor Target | `CitySampleEditor` |
| Autosaves | `Client/Saved/Autosaves/` |

本仓库常用命令（`<repo_root>` 在运行时解析为工作区绝对路径）：

```powershell
& "<repo_root>/Engine/Build/BatchFiles/Build.bat" CitySampleEditor Win64 Development "<repo_root>/Client/CitySample.uproject" -WaitMutex
```

编辑器重启命令（自动跳过 Restore Packages 对话框）：

```powershell
Start-Process -FilePath '<repo_root>\Engine\Binaries\Win64\UnrealEditor.exe' -ArgumentList '<repo_root>\Client\CitySample.uproject','-SkipRestorePackageBackup'
```

启动前先清理 autosave 避免 Restore 弹窗：

```powershell
Remove-Item -Recurse -Force "<repo_root>/Client/Saved/Autosaves/" -ErrorAction SilentlyContinue
```

## 编译并恢复编辑器 Pipeline

```text
1. monolith_status() 检查编辑器是否在线
2. 若在线，先 editor_world_query(get_current_map) 记录当前地图
3. 判断是 Live Coding 还是完整 UBT
4. Live Coding 成功则结束
5. 若需 UBT：关闭编辑器，执行 Build.bat
6. Build 成功后启动 UnrealEditor.exe
7. 轮询 monolith_status() 直到编辑器恢复
8. 若之前记录了地图，调用 editor_world_query(open_map) 恢复
9. 用 monolith_discover() 或 monolith_status() 确认 namespace 正常注册
```

## 快速分诊

| 场景 | 下一步 |
|---|---|
| `.cpp` only 且编辑器开着 | `trigger_build` |
| `.h` / `.Build.cs` / `.uplugin` 改了 | 关闭编辑器，跑 UBT |
| 输出不清楚 | `get_compile_output` |
| 编译错误很多 | `get_build_errors(compile_only=true)` |
| linker / include 错误 | 切到 `unreal-cpp` skill |

## 不要做的事

- 不要在编辑器开着且 Live Coding 激活时强跑 UBT。
- 不要把结构性改动交给 Live Coding。
- 不要先 discover 再 build。