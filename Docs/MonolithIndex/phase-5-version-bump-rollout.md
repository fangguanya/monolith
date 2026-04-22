# MonolithIndex Phase 5 Version Bump Rollout

## 当前状态
- 状态：`待真实 warmup / rollout 数据`
- 本地前置检查（2026-04-22，`R:\city_generator_base`）：
  - `GeneratorEditor Win64 Development` 编译通过。
  - `Automation RunTests Monolith.Index` 全量通过，退出码 `0`。
  - warmup commandlet 相关自动化用例全部通过：
    - `BypassSqlite`
    - `CacheHitRate`
    - `ParseScope`
    - `ReleaseGateHistory`
    - `ReleaseThresholdClamp`
    - `ScopeTargetsIndexer`
    - `TimeWindow`
- 未完成项：
  - 接入真实 Horde / Windows 任务计划入口。
  - 记录连续 warmup run 的命中率数据。
  - 根据命中率门槛执行正式客户端版本放量。

## 版本信息
- Indexer / cohort：
- 旧版本：
- 新版本：
- schema：

## 预热计划
- Horde / 任务计划入口：
- warmup 命令：
- 预热开始时间：

## 命中率门槛
- `WarmupReleaseThreshold`：默认 `90`
- 连续达标次数：默认 `2`

## 发布节奏
1. 先部署到 warmup agent / commandlet。
2. 观察 DDC 预热命中率，至少连续 `2` 次 run 达到 `>=90%`。
3. 达标后发布客户端二进制。

## 回滚
- 触发条件：
- 回滚步骤：

## 记录
- warmup 范围：`-Scope=GlobalReducer` 或 `-Scope=Cohort:<Name>`
- 统计口径：优先记录 `local_hit / remote_hit / remote_miss / remote_write_ok / remote_write_fail`
- 结论：
