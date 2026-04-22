# MonolithIndex Phase 0 根因取证

## 当前状态
- 状态：`待 Unreal Insights / 真实项目 trace`
- 本地前置检查（2026-04-22，`R:\city_generator_base`）：
  - 修复了 full / incremental / live 链路中的后台 `AssetRegistry` 访问；自动化进程里不再触发
    `Enumerating in-memory assets can only be done on the game thread` 断言。
  - `Automation RunTests Monolith.Index` 全量通过，说明索引主链、artifact cache、scheduler、shadow、warmup commandlet 基础行为在本地已稳定。
  - 自动化启动阶段不再拉起项目自动索引，因此本地日志里已不再出现启动期 SQLite 锁表噪音。
- 未完成项：
  - 采集真实项目启动 trace。
  - 量化 GT stall / compile storm / sentinel 或 global 扫描占比。
  - 基于 trace 决定恢复顺序与 cohort 放量节奏。

## 背景
- 目标：确认 GT stall、compile storm、`GetAsset()` 热路径和 sentinel 全局重扫的真实占比。
- 证据目录：`Saved/Profiling/MonolithIndex/`

## Trace 方案
- 恢复 cohort：
- 采样时长：
- 机器：
- CL：

## 观察
### GT stall
- 现象：
- 关键调用栈：

### 编译风暴
- 现象：
- 触发资产类型：

### Sentinel / Global 扫描
- 现象：
- 重扫来源：

## 结论
- 是否允许恢复下一个 cohort：
- 需要回退的 indexer：
- 后续动作：
