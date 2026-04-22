# MonolithIndex Phase -1 Gate 0 报告

## 当前状态
- 状态：`待双机实测签字`
- 本地前置检查（2026-04-22，`R:\city_generator_base`）：
  - `GeneratorEditor Win64 Development` 编译通过。
  - `Automation RunTests Monolith.Index` 全量通过，退出码 `0`。
  - 自动化进程已显式跳过启动自动索引，避免测试临时库与项目库并发争用。
  - full / incremental / live 三条索引写路径已改为“GT 抓取 Asset Registry 快照，后台只消费纯数据”，本地已不再出现后台线程直接枚举 `AssetRegistry` 的断言。
- 未完成项：
  - 两台机器分别导出 identity CSV。
  - 生成并审阅跨机 diff。
  - 根据 diff 结果最终锁定 `SavedHash` 或 `ARSnapshotV1`。

## 目标
- 验证 `FMonolithArtifactIdentityV1` 序列化在两台机器上稳定一致。
- 若 `SavedHash` 方案出现 diff，明确切换到 `ARSnapshotV1`。

## 样本集
- 主样本根路径：`/Game/Characters`
- 回退根路径：`/Game`
- 样本数量：`1000`

## 产物
- 机器 A CSV：
- 机器 B CSV：
- diff 文件：

## provider 决策
- 初始 provider：
- diff 行数：
- 最终锁定 provider：

## 结论
- [ ] Gate 0 通过，可继续 Shared DDC
- [ ] Gate 0 未通过，已切换到 `ARSnapshotV1`
- [x] 本地代码前置条件已满足，等待双机 CSV 实测

## 签字
- Reviewer：
- Date：
