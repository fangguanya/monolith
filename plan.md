# Monolith Index / Deep Index 重构方案 v6（plan.md 完整替换稿）

## Summary
- 本方案只覆盖 `MonolithIndex` 资产索引链路；`MonolithSource` 本轮 **不迁移、不合库、不共 bucket**。若后续接 DDC，单独走 `MonolithSourceV1`。
- 共享缓存首版统一走 UE 官方 `DerivedDataCache` API，不直接调 raw Zen RPC；SQLite 继续只做本地查询快照和 materialized rows。
- SQLite 维持 `journal_mode=DELETE`、`synchronous=NORMAL`，本轮不切 WAL。
- 查询永不因索引中而拒绝服务；统一返回最近一次已提交快照，并附带 `indexing_in_progress / stale / remaining_items / eta_seconds / cache stats`。
- 默认 `bEnableIndex=true`；仅在 Project Settings 显式关闭时为 `false`，CI/自动化可用 `-nomonolithindex` 覆盖。
- 单机开发者可先跳过跨机 Gate 0，但 **在启用 Shared DDC 之前** 必须补做两机 Gate 0 报告并签字。
- 落地顺序固定为：`Phase -1 Gate 0` -> `Phase 0 根因取证+灰度恢复` -> `Phase 1 非阻塞调度` -> `Phase 2a 接口扩展` -> `Phase 2b cohort 迁移` -> `Phase 2c Mesh 视觉查询扩展（单独 spec 批准后进入实施）` -> `Phase 3 DDC 共享` -> `Phase 4 Live/状态栏/语义` -> `Phase 5 压缩/Horde/滚动升级`。

## 根因分析
- 已确认问题链路：
  - 后台索引线程仍依赖 GT 回调并阻塞等待，导致“线程在后台，卡顿在前台”。
  - full 和 incremental 都直接 `GetAsset()`，把 UObject load 拉进 GT 热路径。
  - sentinel / global reducer 存在全局重扫，重复计算远大于普通 class dispatch。
  - 为止血，full / incremental / live 三条链路已被直接熔断。
  - `project.search` 当前在索引期间直接返回失败，违背“主逻辑不阻塞”目标。
- Phase 0 必须补齐的证据：
  - `GetAsset()` 是否触发 texture compiler / shader compile / Blueprint recompile / load stall。
  - 哪些 indexer 的单 job 超过可接受 GT 预算。
- 根因文档与证据固定落地：
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-0-root-cause.md`
  - `Saved/Profiling/MonolithIndex/*.utrace`

## Implementation Changes
### Phase -1 / Gate 0：Identity POC
- 样本集固定为：优先 `/Game/Characters` 下按包名排序前 `1000` 个资产；不足则回退到 `/Game` 前 `1000` 个。
- 两台机器同步同一 CL，生成 CSV：`package_path, provider, identity_hash, saved_hash`。
- `Serialize(FMonolithArtifactIdentityV1)` 必须使用显式字段序列化：手写 builder 写入 `TArray<uint8>`，不得依赖默认 `FArchive operator<<`、`UStruct` 反射序列化或 `StructOpsTypeTraits`。
- identity 序列化规则固定为：
  - 字段按 `FMonolithArtifactIdentityV1` 声明顺序写入
  - 所有整数写 little-endian
  - enum 写固定 `uint8`
  - `FName` 只写 `DisplayString`，不写 number、comparison index 或运行时内部 id
  - string 写 length-prefixed UTF-8 bytes
  - array/map 必须先按 canonical key 排序再写入
- Gate 的唯一通过凭据不是 CI，而是人工报告：
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-minus-1-gate0-report.md`
  - 必须附：机器 A/B CSV、diff 结果、最终 provider 锁定决定、签字结论。
- 默认 provider 为 `SavedHash`；若两机 diff 非 0，立即切到 `ARSnapshotV1`：
  - `PackageFingerprint = SHA1(sorted tag pairs + sorted AR hard deps + sorted AR soft deps + schema_version)`
  - `ArtifactIdentityHash = Blake3(Serialize(FMonolithArtifactIdentityV1))`
- Gate 通过后把 provider 固定为项目配置；后续阶段不得漂移。
- 单机场景允许暂时默认 `SavedHash`，但在启用 Shared DDC 前必须补做两机 Gate 0 报告。
- 自动化测试只验证代码路径，不宣称“跨机 Gate 通过”：
  - `test_identity_serialization_is_deterministic_single_process`
  - `test_provider_switch_picks_ar_snapshot_when_setting_set`

### Phase 0：根因取证 + 灰度恢复
- 禁止“一把把 7 个熔断全开回去”。
- 恢复顺序固定为：`Dependency -> GameplayTags -> DataTable -> Animation -> Niagara -> MeshCatalog -> Level`。
- 先只恢复 `DependencyIndexer`，跑 `5` 分钟 trace，产出 `phase-0-root-cause.md`。
- 每恢复一个 cohort，必须通过：
  - 无新增 catastrophic GT stall
  - 无 compile storm
  - query 不回归
- 若某个 indexer 恢复后出现 `GT > 100ms` 或编译风暴，只回退该 indexer，不回退整个阶段。
- `bEnableIndex` 接成真实 kill switch；关闭时完全不启动索引执行链。
- `MonolithQuery` CLI 与只读查询链保持兼容。

### Phase 1：非阻塞调度器 + GT 保护 + 关机语义
- 新增 `FMonolithIndexScheduler`，替换 `FRunnable + Event->Wait()` 主循环。
- 资源池固定为：
  - `BackgroundCpuPool`
  - `IoDdcPool`
- 不再保留独立 `GameThreadPumper`；生产索引链禁止通过专用 GT wait/pump 通道回切主线程。
- `IoDdcPoolSize` 默认 `2`，可通过 `MonolithSettings.IoDdcPoolSize` 调整到 `4`。
- 启动时 DDC -> SQLite materialize 不阻塞编辑器启动：所有 bulk materialize 进入 `BackgroundCpuPool`，批大小固定 `500 packages / transaction`；查询层立即可用，尚未 materialize 的 package 返回 `stale=true`。
- 不做中途硬杀 load job；改为后验 watchdog：
  - job 完成后若耗时 `>100ms`，记 `gt_overrun_count`
  - 同类 indexer/job class 立即标为 `GTQuarantined`
  - 后续同类任务默认降级为 `AROnly` 或 `OfflineOnly`
  - 连续 `3` 次 overrun，GT breaker 打开 `30s`
- `OfflineOnly` 不由编辑器消费；统一落入持久化离线队列，由 **本轮新增** 的 `UMonolithIndexWarmupCommandlet` 或 Horde batch job 消费。
- 指标语义固定：
  - `gt_overrun_count`：单 job 耗时 `>100ms` 的事件次数，可在同一 cohort 内多次增长
  - `gt_downgrade_count`：某个 cohort/job class 首次被标为 `GTQuarantined` 的次数，每个 cohort/job class 最多增长一次
- commandlet 固定新增位置：
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Commandlets\MonolithIndexWarmupCommandlet.cpp`
- commandlet 参数固定支持：
  - `-Scope=OfflineOnly|Cohort:<Name>|All`
  - `-Priority=Background`
  - `-TimeWindowMinutes=<N>`
  - `-MaxPackages=<N>`
- commandlet 调度方式固定支持：
  - Horde job
  - Windows 任务计划
  - 手动命令行
- **DB 互斥策略固定**：commandlet **只写 DDC，不写本地 SQLite**；编辑器启动后从 DDC 命中再 materialize 到自己的 SQLite。SQLite 始终只有编辑器进程写。
- 关机语义固定为：
  - 停止接收新任务
  - 必须 drain 当前本地 DB materialize，保证 SQLite 一致性
  - `Put(StoreRemote)` 队列允许丢弃
  - 全局 drain 硬上限 `5s`，超时直接退出
- 新增指标：
  - `gt_overrun_count`
  - `gt_downgrade_count`
  - `gt_breaker_open`
  - `gt_breaker_remaining_seconds`

### Phase 2a：非破坏接口扩展 + 单 cohort shadow mode
- `IMonolithIndexer` 新增默认实现接口：
  - `GetIndexerId()`
  - `GetIndexerVersion()`
  - `GetExecutionMode()`
  - `BuildArtifact()`
  - `MaterializeArtifact()`
  - 旧 `IndexAsset()` 保留
- 调度器规则：
  - 实现了 artifact 路径的 indexer 走新链路
  - 未实现的仍走旧 `IndexAsset`
- shadow mode 约束固定为：
  - 任意时刻最多 `1` 个 cohort 处于 shadow 状态
  - 由 `bEnableArtifactShadowMode=true` + `ShadowModeCohort=FName` 强制控制
  - shadow mode 只跑 `Background` 优先级
  - `Interactive/Live` 始终走单一路径，不允许双跑

### Phase 2b：按 cohort 灰度迁移 + 明确定义 diff + shadow 生命周期
- cohort 顺序固定为：
  1. `Blueprint`
  2. `Material`
  3. `Dependency`
  4. `Level`
  5. `DataTable / GameplayTags / Animation`
  6. 其他 asset indexer
- 每次只迁一个 cohort；完成 `shadow -> diff -> rollback window -> promote` 四步后再迁下一个。
- shadow table 规则固定为：
  - 命名：`shadow_<cohort>_<table>`
  - 建表时机：进入该 cohort shadow mode 时创建
  - promote 成功后保留 `24h`，随后自动 `DROP`
  - rollback 场景下保留 `7d` 供人工调查，再自动 `DROP`
- diff 固定两层：
  - `Level 1` 快速 diff：按表比较 `COUNT(*)` 与 `SUM(row_hash)`；`row_hash` 只存在于 shadow tables，不污染生产表
  - `Level 2` 下钻：仅当 Level 1 不相等时触发，按主键做 **确定性采样** `ABS(hash(pk)) % 100 = 0`，对 `1%` 行做字段对比
- `row_hash` 计算规范固定为 `XXHash64(canonical_bytes(columns))`：
  - `NULL` 写入固定 tag：`\x00NULL\x00`
  - empty string 写入 string tag + length 0，必须与 `NULL` 区分
  - `INTEGER` 写入 little-endian 64-bit
  - `REAL` 写入 little-endian IEEE 754；所有 NaN 归一为同一个固定 quiet-NaN 字节序列
  - `TEXT` 使用 `FTCHARToUTF8` 转 UTF-8 后写入 length-prefix bytes，不强制 NFC/NFD Unicode 规范化；当前团队默认全 Windows + UE 5.7，该行为可接受。跨平台场景必须补 Unicode normalization 层
  - `BLOB` 写入 length-prefix 原始字节
  - column 顺序固定为 schema 定义顺序，不能使用 `SELECT *` 的隐式顺序
- `0.1%` 阈值定义为：**Level 1 聚合差异比例**，不是全量 row-by-row 差异比例。
- 回滚条件固定为：
  - Level 1 聚合差异比例 `>0.1%`
  - GT overrun 显著增加
  - cache write 放大异常

### Phase 2c：Mesh 视觉查询扩展（已完成 v1 实施 — 2026-04-25）

**v1 已落地结果**：
- `monolith-mesh-visual-query` spec 已合入 `openspec/changes/`，验证通过
- 双 cohort：`AssetVisualGeometric`（geometric_v1，64 维 FP32，单 iso 视图 + silhouette 编码）+ `AssetVisualSemantic`（clip_vit_b32_v1，512 维 FP16，CLIP-ViT-B/32 NNE GPU 推理）
- Sharding 落地：`/Game/<L1>/<L2>` 起步，超容量自动按下一段再拆，geometric ≤50K/shard、semantic ≤8K/shard
- ANN：sharded brute-force cosine（无第三方库依赖），性能基准 30ms 内通过
- DDC 双 bucket：`MonolithAssetVisualGeometricV1` + `MonolithAssetVisualSemanticV1`，与 `MonolithIndexV2` 物理隔离
- canonical render：复用 `MonolithCapture/Public/MonolithCaptureUtils.h::IAssetCanonicalRenderer`，每 mesh 持久化 1 张 256×256 iso PNG（双 cohort 共享）
- semantic provider：通过 NNE + ONNXRuntime 调用 `Plugins/Monolith/Resources/Models/clip_vit_b32_image_encoder_fp16.onnx`；VRAM 1.5GB 上限 + PIE 自动暂停
- 三个 action：`mesh.get_selected_mesh_assets` / `mesh.query_mesh_under_cursor`（GT，p95 ≤5/16ms）+ `asset.search_assets_by_image`（BackgroundCpuPool，provider=auto/geometric/semantic/both，p95 ≤500ms）
- 状态接入：`project.get_stats` 增 `asset_visual_geometric_row_count` / `asset_visual_semantic_row_count`；`project.list_stale_packages` 增 `cohort` 参数
- Warmup commandlet：`-Scope=Cohort:AssetVisualGeometric|AssetVisualSemantic` + `-ShardRange=<begin>:<end>`（Horde 多机切片）

### Phase 2c（历史规划稿，已被上面 v1 实施替代）
- 本阶段目标不是替换 `MeshCatalog`，而是在其上方新增独立能力：
  - 编辑器上下文直达：`mesh.get_selected_mesh_assets`
  - 编辑器命中直达：`mesh.query_mesh_under_cursor`
  - 图片检索：`asset.search_assets_by_image`
- 架构约束固定为：
  - 新增独立 companion cohort：`AssetVisual`
  - `MeshCatalog` 继续只承载结构化范围检索字段，不追加 embedding/vector 列
  - visual query 统一走 `visual ANN recall -> MeshCatalog rerank`
  - canonical render 统一复用 `MonolithCapture` 的 shared preview helper，不得维护第二套 StaticMesh 预览渲染实现
- `AssetVisual` artifact 固定至少包含：
  - `front / side / back / top / iso` canonical views
  - 单一 configured embedding provider/version
  - `render_recipe_version`
- 图片查询输入固定支持：
  - `image_path` 或 `image_base64`
  - 可选 `bbox` / `mask`
  - 可选 `category_hint / size_hint`
- 查询结果固定返回：
  - `asset_path`
  - `preview_view`
  - `visual_score`
  - `rerank_score`
  - `total_score`
  - `stale`

### Phase 3：DDC 共享缓存 + 上层熔断
- 当前非兼容 artifact 编码升级后的 DDC bucket 固定为 `MonolithIndexV2`；未来若再有非兼容 schema，直接开 `MonolithIndexV3` 并行，旧 bucket 自然 TTL 淘汰。
- DDC 映射固定为：
  - `FCacheBucket("MonolithIndexV2")`
  - `FCacheKey.Hash = Blake3(Serialize(FMonolithArtifactIdentityV1))`
  - `FValueId = FValueId::FromName("artifact")`
- `FCacheRecord.Meta` 固定第一个字段为 `meta_schema:uint8`，后续字段为：
  - `indexer_id`
  - `indexer_version`
  - `artifact_schema_version`
  - `package_name`
  - `identity_provider`
  - `build_ms`
- 反序列化必须先读 `meta_schema`，再按版本读 body。
- `package_name` 在 Meta 中仅作调试和观测用途；**权威身份始终是 `FCacheKey.Hash`**。若同内容被不同路径写入，Meta 的 `package_name` 以后写者为准，不影响正确性。
- 读路径固定为两段：
  - `Get(Local)` -> `local_hit`
  - miss 后 `Get(QueryRemote | StoreLocal)` -> `remote_hit / remote_miss`
- 写路径固定为两段：
  - `Put(StoreLocal)`
  - 异步 `Put(StoreRemote)` -> `remote_write_ok / remote_write_fail`
- Monolith 在 DDC 之上再包一层 breaker：
  - `Interactive/Live` remote get 超时 `250ms`
  - `Background` remote get 超时 `1000ms`
  - remote put 超时 `2000ms`
  - 连续 `5` 次错误或 `30s` 内累计 `8` 次错误，打开 breaker `60s`
  - breaker 打开期间标记 `remote_disabled`
  - half-open 只放行 `1` 次 get probe + `1` 次 put probe；都成功才恢复

### Phase 4：Live 原子切换 + stale 语义 + 状态栏
- live 路径禁止“先删后插”；改为“新 revision 写入 -> current pointer 原子切换”。
- DB migration 固定为：
  - `ALTER TABLE assets ADD COLUMN current_revision_id INTEGER DEFAULT 0`
  - 老行 `current_revision_id=0` 视为 `pre-revision epoch`
  - Phase 4 部署后首次 live/incremental 写入时自动升级为非零 revision
  - 不因该列迁移强制全量重建
- 数据模型固定为：
  - `assets.current_revision_id`
  - materialized rows 全带 `revision_id`
- `assets` 表写入固定走 `INSERT ... ON CONFLICT(package_path) DO UPDATE`
- stale 语义固定拆成两层：
  - `indexing_in_progress = (queue_depth > 0)`
  - package 级 `stale = (package in queue/running) OR (artifact.indexer_version < current_indexer_version) OR (artifact.identity_provider != current_configured_provider)`
- `identity_provider mismatch` 视为 **provider 级失效**，与 `IndexerVersion` 失效同级；含义是“本地 DB 行过期，需要重新走 DDC 或本地重算”，不是 Gate 0 配置漂移。
- `project.search`：
  - 顶层返回 `indexing_in_progress`
  - 每条结果返回 `stale`
  - 顶层 `stale = any(result.stale)`
- `project.get_stats` 返回：
  - `indexing_in_progress`
  - `stale_packages`：`uint32` 计数，不返回大列表
  - `queue_depth`
  - `remaining_items`
  - `eta_seconds`
  - `local_hit / remote_hit / remote_miss / remote_write_ok / remote_write_fail`
  - `gt_overrun_count / gt_downgrade_count / remote_disabled`
- 若需要具体 stale package 列表，新增独立分页 API：`project.list_stale_packages(limit, cursor)`。
- 状态栏 widget 固定 `0.2s` 刷新，详情面板固定 `0.5s` 刷新，常驻展示：
  - `Index X/Y`
  - `left Zm`
  - `local A%`
  - `remote B%`
  - `miss C%`
  - `write N MB`
  - `GT breaker`
  - `remote breaker`

### Phase 5：Artifact 编码 + GlobalReducer + Horde 滚动升级
- artifact payload 默认统一压缩后再入 DDC。
- **按当前本地 UE 源码**，实现首选 `FValue::Compress(const FSharedBuffer& RawData, uint64 BlockSize=0)` 或 `FValue::Compress(const FCompositeBuffer& RawData, uint64 BlockSize=0)`；实施 PR 时以 [DerivedDataValue.h](/E:/fanggang_matrix/Unreal_Matrix/Engine/Source/Developer/DerivedDataCache/Public/DerivedDataValue.h#L32) 为准。
- 尺寸策略固定为：
  - 压缩后 `<=4MB`：单 value
  - 压缩后 `>4MB && 原始 <=16MB`：拆为 `header + chunk.N`
  - 原始 `>16MB`：不进共享缓存，只做本地 materialize，并记 `oversized_artifact`
- `IndexerVersion` 只表示“逻辑变更”。
- `DependencySalt` 只包含：
  - `EngineMajorMinor`
  - `TArray<FMonolithDependencyVersion>`，由 indexer 显式声明
- `EngineMajorMinor` 在 `SavedHash` provider 下通常冗余，因为 saved hash 已能捕捉 engine 版本导致的 package 变化；但在 `ARSnapshotV1` provider 下不走 saved hash，必须由 salt 兜底。因此统一保留，保证两种 provider 都对 engine 大小版本敏感。
- `MonolithPluginVersion` 不进入 salt。
- `GlobalReducer` 迁移规则固定为：
  - reducer identity 不直接用全库时间点，而用 `UpstreamManifestHash`
  - `UpstreamManifestHash = Blake3(sorted list of (package_path, upstream_artifact_key_hash))`
  - `ReducerIdentity = Blake3(IndexerId + IndexerVersion + SchemaVersion + UpstreamManifestHash)`
  - `GlobalReducer` 只在 Horde 预热或手动触发时跑
  - 编辑器 live 路径 **不跑 reducer**，只消费最后一次 reducer 快照
- `GlobalReducer` 默认触发周期：
  - 有 Horde：每天 `02:00` 跑一次，并在每次 `main` 合并后触发一次
  - 无 Horde：通过 Windows 任务计划调用 `MonolithIndexWarmupCommandlet -Scope=GlobalReducer`
- 依赖 `GlobalReducer` 输出的查询结果，在 live 路径上默认携带 `stale=true`，直到下一次 reducer run 完成并原子替换快照。
- indexer version bump 的 rolling upgrade 协议固定为：
  1. 新二进制先只部署给 Horde warmup agent / commandlet
  2. Horde 先预热新 version artifact
  3. 生成 `phase-5-version-bump-rollout.md`
  4. 预热命中率达标后再发布客户端二进制
- “预热命中率达标”默认定义为 `>=90%`，可通过 `MonolithSettings.WarmupReleaseThreshold` 调整；且必须连续 `2` 次 warmup run 都达标才允许发布客户端二进制。
- bump 与 warmup 解耦，避免 DDC 雷暴。

## Public APIs / Types
- `enum class EMonolithIdentityProvider { SavedHash, ARSnapshotV1 }`
- `struct FMonolithDependencyVersion { FName Id; uint32 Version; }`
- `struct FMonolithArtifactIdentityV1`
- `struct FMonolithArtifact`
- `enum class EMonolithExecutionMode { AROnly, PackageScopedLoad, GlobalReducer, OfflineOnly }`
- `class IMonolithArtifactCache`
- `class FMonolithDdcArtifactCache final`
- `IMonolithIndexer` 新接口全部提供默认实现，本轮保留 `IndexAsset()`

## Test Plan
- **人工 Gate 凭据**
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-minus-1-gate0-report.md`
  - 这是 Gate 0 通过的唯一凭据；CI 绿灯不代表 Gate 通过
- **自动化测试文件与函数**
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\ArtifactIdentity_test.cpp`
    - `test_identity_serialization_is_deterministic_single_process`
    - `test_provider_switch_picks_ar_snapshot_when_setting_set`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\MonolithIndexScheduler_test.cpp`
    - `ClampConfig`
    - `DrainHonorsCooperativeStop`
    - `HardCapTimeoutCanBeRecovered`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\MonolithIndexGtBudget_test.cpp`
    - `BreakerOpensAfterThreeOverruns`
    - `DowngradeCountsPerIndexer`
    - `ThrottleAppliesToQuarantinedIndexer`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\ArtifactCache_DDC_test.cpp`
    - `LocalRoundTrip`
    - `ChunkedRoundTrip`
    - `OversizedArtifactsSkipCache`
    - `BreakerOpensAfterConsecutiveErrors`
    - `HalfOpenProbeRecoversRemotePath`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\MonolithIndexDatabaseRevision_test.cpp`
    - `RevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `RevisionDiscardPreservesPreviousSnapshot`
    - `ActorRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `ActorRevisionDiscardPreservesPreviousSnapshot`
    - `DataTableRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `DataTableRevisionDiscardPreservesPreviousSnapshot`
    - `DependencyRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `DependencyRevisionDiscardPreservesPreviousSnapshot`
    - `GameplayTagReferenceRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `GameplayTagReferenceRevisionDiscardPreservesPreviousSnapshot`
    - `MeshCatalogRevisionSwitchKeepsPreviousSnapshotVisibleUntilPromote`
    - `MeshCatalogRevisionDiscardPreservesPreviousSnapshot`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\ProjectQueryPayload_test.cpp`
    - `SearchAggregatesStaleAndProgress`
    - `StatsKeepsIndexingSeparateFromStale`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\MonolithIndexRuntimeStateTests.cpp`
    - `StaleSemantics`
    - `StalePagination`
    - `PrefixMatching`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\IndexerShadowMode_test.cpp`
    - `AllowsOnlyOneCohortAtATime`
    - `UsesLevel1ThenLevel2Diff`
    - `Level2SamplingIsDeterministic`
    - `ShadowTablesDropAfterPromoteRetention`
  - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\WarmupCommandlet_test.cpp`
    - `ParseScope`
    - `TimeWindow`
    - `BypassSqlite`
    - `ReleaseThresholdClamp`
- **验收门槛**
  - Gate 0 两机 diff `= 0` 或报告明确锁定 `ARSnapshotV1`
  - query 在索引中不拒绝服务
  - GT 无 `Wait()` 型阻塞路径
  - remote 失败时 local-only 路径稳定
  - `MonolithQuery` CLI 兼容不回归

## Assumptions / Risks / Compatibility
- `MonolithSource` 本轮明确不迁移；若后续迁移，走独立 bucket `MonolithSourceV1`。
- Shared DDC/Zen 若不存在，系统退化为 local-only，正确性仍成立。
- 本轮不改 WAL。
- 关机时允许丢 remote put，不允许丢已进入本地 DB 提交的 materialize。
- `OfflineOnly` 队列由 `UMonolithIndexWarmupCommandlet` 或 Horde 消费；编辑器本身不消费。
- 任何 schema 变化都必须保证 `MonolithQuery` CLI 与只读查询链继续可读，或提供等价兼容视图。
- 测试目录采用 plugin-local 路径 `Client/Plugins/Matrix/Monolith/Source/MonolithIndex/Private/Tests/`。这是 Monolith 对 ccgs 测试路径规则的明确例外，理由是该批测试属于 UE Automation plugin-local 单元/集成测试；实现前需让测试 reviewer 确认该例外。
- `plan.md` 落地后必须同步镜像或迁移到 `openspec/specs/adr/ADR-monolith-index-v2.md`，作为项目级 ADR 入口；`plan.md` 在 Phase 5 完成后归档为实施稿。

## Review 结论 / 迭代收敛记录

本方案历经 v1 → v6 共 6 轮评审迭代。每轮评审的采纳率：

| 轮次 | 评审提出项 | 采纳 | 剩余项性质 |
|---|---|---|---|
| v1 → v2 | 4 项决策（WAL 陷阱 / Zen→DDC / Horde 降级 / saved_hash Gate）+ 11 项 gap | 3 决策 + 部分 gap | 有 P0 合规风险（WAL 陷阱） |
| v2 → v3 | 11 项 gap | 11/11 | 方向性问题全部解决 |
| v3 → v4 | 10 项 tactical gap | 9/10 | Public API 头文件落位（PR 补） |
| v4 → v5 | 10 项 minor gap | 10/10 | — |
| v5 → v6 | 10 项 cosmetic gap | 10/10（含 ADR 落位补齐） | — |

### v6 方案最终状态

- **架构正确性**：已核对 UE 5.7 源码与本项目现状，无 P0 合规风险。WAL 陷阱已规避（保持 DELETE），DDC 走官方 `IDerivedDataCache` 接口，Horde 仅做预热不做实时 RPC，`SavedHash` provider 有 Gate 0 跨机验证 + `ARSnapshotV1` fallback。
- **五步论证对齐**（`ccgs-modification-standards.md`）：问题 / 根因 / 方案 / 验证 / 禁止事项 全部覆盖；每个 Phase 的具体 PR 仍需各自附带五步论证。
- **测试证据**（`ccgs-coding-standards.md`）：自动化测试文件与函数名全部落地；人工 Gate 0 凭据与自动化测试明确分离；plugin-local 测试路径例外已在 Assumptions 标注。
- **向后兼容**：`MonolithQuery` CLI 兼容承诺 / `MonolithSource` 明确出范围 / SQLite 不切 WAL / `IndexAsset()` 保留。
- **ADR 落位**：已在 Assumptions 明确 `openspec/specs/adr/ADR-monolith-index-v2.md` 镜像路径，满足 `ccgs-coding-standards.md` 强制要求。

### 2026-04-25 新增收敛结论（图片 / mesh 查询）

- 当前 `MeshCatalog` 的职责保持不变：只负责尺寸、类别、LOD、碰撞等结构化字段检索，不承担图片 / 向量检索。
- “编辑器里已经看到或选中的物体是谁”优先走上下文直达，不走图片检索。
- 图片找 mesh 的正式方案必须新增独立 `AssetVisual` cohort，并采用 `canonical render -> single embedding provider -> ANN recall -> MeshCatalog rerank` 的唯一链路。
- `MonolithCapture` 现有 static mesh 离屏预览能力必须被抽成共享 helper，供 action 和 indexer 共用，禁止再维护第二套预览渲染实现。

### v6 残留 tactical gap（不 block 实施，PR 时补）

按实施阶段排序：

| ID | 项 | 补位时机 |
|---|---|---|
| A | `Serialize(FMonolithArtifactIdentityV1)` 必须手写显式字段序列化，禁用默认 `FArchive operator<<` 与 `StructOpsTypeTraits`；字段按声明顺序 + little-endian + FName 只写 DisplayString。这是 Gate 0 跨机稳定性的生死线 | Phase -1 实施 PR |
| D | 启动时 DDC→SQLite bulk materialize 策略：走 `BackgroundCpuPool`，批大小 `500/事务`；查询层立即可用，未 materialize 的包返回 `stale=true` | Phase 1 实施 PR |
| H | `test_commandlet_never_opens_local_sqlite` 验证方式：断言 DB 文件 mtime 未变 + 无 DB open call；注意读 `MonolithSettings` 不得间接触碰 Saved/Config 的 DB 路径 | Phase 1 实施 PR |
| B | `TEXT` 转 UTF-8 使用 `FTCHARToUTF8`，不强制 NFC 规范化；团队全 Windows + UE 5.7 下稳定；跨平台场景需补规范化层 | Phase 2b 实施 PR |
| I | `row_hash` column 顺序：启动时 cache 每表 schema order，避免每次 diff 重新 parse `sqlite_master.sql` | Phase 2b 实施 PR |
| F | `project.list_stale_packages(limit, cursor)` 游标语义：opaque string，服务端编码 | Phase 4 实施 PR |
| C | `GlobalReducer` 默认触发周期："每晚 02:00 + 每次 main 合并后" 由 Horde 触发；无 Horde 时通过 Windows 任务计划调 `MonolithIndexWarmupCommandlet -Scope=GlobalReducer` | Phase 5 实施 PR |
| E | `MonolithSettings.WarmupReleaseThreshold` 单位固定为百分比 int（`0-100`） | Phase 5 settings |
| G | `phase-5-version-bump-rollout.md` 落位：`E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-5-version-bump-rollout.md` | Phase 5 实施 PR |

### Merge 判定

**批准进入实施阶段。** 继续在 `plan.md` 上打磨字面已无新信息增益；所有剩余问题必须通过代码验证，而非评审推理。

### 立即下一步（并行可做）

1. **Phase -1 / Gate 0 POC**（零侵入，纯新增）
   - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tools\MonolithIdentityPocCommandlet.cpp` —— 一次性 commandlet，读 1000 asset 输出 CSV
   - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-minus-1-gate0-report.md` 模板
   - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Source\MonolithIndex\Private\Tests\ArtifactIdentity_test.cpp` 两个自动化测试
   - 实施时附带残留 gap §A 的显式序列化约束

2. **Phase 0 根因取证 SOP**
   - `E:\fanggang_matrix\Unreal_Matrix\Client\Plugins\Matrix\Monolith\Docs\MonolithIndex\phase-0-root-cause.md` 模板
   - 按 `matrix-pie-startup-profiling` skill 的多源 SOP 组织
   - Unreal Insights trace 采集脚本 + 5 分钟 baseline trace

3. **ADR 镜像**
   - `E:\fanggang_matrix\Unreal_Matrix\openspec\specs\adr\ADR-monolith-index-v2.md` 按 8 段式设计文档标准写
   - `plan.md` 作为实施稿被 ADR 引用

4. **图片 / Mesh 查询 spec**
   - `R:\city_generator_base\openspec\changes\add-monolith-mesh-visual-query\`
   - 先走 spec / proposal / design / tasks 审核，再进入 `Phase 2c`

三项无相互依赖，可并行起步。建议先做 **1 + 2**，因为它们是所有后续 Phase 的前置门禁；**3** 可在任何时候补齐。
