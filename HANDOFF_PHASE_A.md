# Phase A 交接提示词

把以下内容**整段**粘到新 session 的第一条消息。

---

# 当前状态（2026-04-28 更新，"已落地 Phase A 多 phase 全套代码"）

✅ 数据层 / 渲染层 / indexer 层 / 搜索层 4 步**已写完并编译通过**。代码在工作树上还**没 commit**。

❗ **没跑过 Materialize 全量验证** —— 端到端"实际产出 phase>0 行 + 用 anim middle-frame PNG 搜中"还需要用户手动跑。这是新 session 接手时的第一件事。

## 已交付改动（按层次列）

| 层 | 文件 | 关键改动 |
|---|---|---|
| 数据 | `Source/MonolithIndex/Public/AssetVisualEntry.h` | `FIndexedAssetVisualEntry` 加 `PhaseId` / `PhaseT` / `PhaseLabel` |
| 数据 | `Source/MonolithIndex/Private/MonolithIndexDatabase.cpp` | bootstrap CREATE 加 3 列；v10→v11 ALTER + DELETE 全行；Insert/Get/ReplaceShadow 同步；新增 `GetAssetVisualEntriesForAsset` 多 phase 读；`GetProductionAssetVisualAggregateForAsset` 改为迭代全 phase |
| 数据 | `Source/MonolithIndex/Private/MonolithIndexerShadowMode.cpp` | `ComputeAssetVisualRowHash` 把 PhaseId/PhaseT/PhaseLabel 纳入 |
| 数据 | `Source/MonolithIndex/Private/AssetVisualArtifact.{h,cpp}` | payload schema **v3**：每 phase 携带独立 PNG（与 handoff 原 §7 的 disk N 张 PNG 自洽）；`SerializePayload(TArray<Entry>, TArray<TArray<uint8>>)` |
| 渲染 | `Source/MonolithCapture/Public/MonolithCaptureUtils.h` | `FCanonicalRenderRequest` 加 `PhaseT` / `PhaseId` / `AssetClassHint` |
| 渲染 | `Source/MonolithCapture/Private/AssetCanonicalRenderer.cpp` | `RenderRecipeVersion` 4→5；新增 `RenderAnimSequencePhasePipeline`（FPreviewScene + UAnimSingleNodeInstance + SetPosition + RefreshBoneTransforms）/ `RenderNiagaraSystemPhasePipeline`（NiagaraComponent + AdvanceSimulation 至 PhaseT 秒）；`RenderPrimitiveComponentToImage` 加可选 `PreCaptureSetup` 回调 |
| Indexer | `Source/MonolithIndex/Private/Indexers/AssetVisualPhase.h`（新文件）| `GetPhasesForAsset(Asset)` 返回 phase 列表（Anim 25/50/75%；Niagara 0.5s/1.5s/3.0s；其余单 phase）；`GetRendererAssetClassHint(Asset)` 给 renderer 路径分发用 |
| Indexer | `AssetVisualGeometricIndexer.{h,cpp}` | `BuildArtifact` 循环 phase 渲染 + Encode；`MaterializeArtifact` 落 N 张 PNG（`Anim_Walk_p0/p1/p2.png`）；`IndexerVersion` 14→**16** |
| Indexer | `AssetVisualSemanticIndexer.{h,cpp}` | `BuildArtifact` 循环 phase + FP16 编码；`MaterializeArtifact` 按 PhaseId 在 geometric cohort 找匹配 preview path；`IndexerVersion` 14→**16** |
| 搜索 | `Source/MonolithIndex/Public/AssetVisualShardedRetriever.h` | `FAssetVisualShardEmbeddings` 加 `RowPhaseIds`；`FAssetVisualRetrieverHit` 加 `PhaseId` |
| 搜索 | `Source/MonolithIndex/Private/AssetVisualShardedRetriever.cpp` | 命中时把行 PhaseId 拷进 hit |
| 搜索 | `Source/MonolithIndex/Private/Reducers/AssetVisualShardReducer.cpp` | shard payload schema **v1→v2**：每行追加 uint8 PhaseId |
| 搜索 | `Source/MonolithMesh/Private/MonolithAssetVisualSearchActions.cpp` | `LoadCohortShardEmbeddings` 填 `RowPhaseIds`；候选用 `TMap<AssetPath, FCandidate>` dedup（每 (asset, provider) 取最高分 phase）；JSON 加 `best_phase_id` / `best_phase_t` / `best_phase_label` |
| 测试 | 4 个 .cpp 测试更新 + 新增 multi-phase round-trip + retriever PhaseId 单测 | |

## 与原 handoff 文档**实际交付的偏离**（要看的话）

1. **`IndexerVersion` 走到 16，不是 doc 原写的 15**。原因：step 1 已经因 payload v1→v2 bump 到 15；step 3 因为 `RenderRecipeVersion` 4→5 不进 identity 必须再 bump 一次（不然 DDC 旧 v15 单 phase 字节流以同 identity 命中 → MaterializeArtifact deserialize 失败 → 永远不重 build）。
2. **Payload schema 走到 v3，不是 doc 原计划的 v2**。原因：doc §7 写"每 phase 一张 PNG 落盘 (Anim_Walk_p1.png)" 与 doc schema 表"末尾共享一张 PNG" 互相矛盾。per-phase PNG 是唯一自洽方案，schema 升 v3 同时让 v2 中间格式被 reader 拒收。
3. **Shard payload schema v1→v2**（doc 说"reducer 不需要改"）：算法没变，但每行多写一个 uint8 PhaseId 让 retriever 能把命中映射回相位；不改 schema retriever 拿不到 PhaseId 没法 dedup。
4. **新增 `GetAssetVisualEntriesForAsset` 而不是改 `GetAssetVisualEntryForAsset` 签名**：保留单行 API（`ORDER BY phase_id ASC LIMIT 1` 取主 phase）兼容历史调用方；新方法给多 phase 用例。同步把 `GetProductionAssetVisualAggregateForAsset` 改成迭代全 phase 修了 shadow Level-1 diff 漏掉局部 phase 变化的潜在 bug。

## 还没做的事（接手第一件事就是这个）

按 doc 的 Step 5 测试用例跑一遍：

```sh
# 1. 重启编辑器（v10→v11 migration 自动跑：ALTER 加 3 列 + DELETE 全行 + DELETE 对应 asset_index_metadata）

# 2. 工具栏 → Monolith → Materialize 全部 (并行 4 进程)
# 24K 资产 + anim/niagara 3x 行膨胀 ≈ 30K 行 / cohort，~25 分钟

# 3. 验证 SQLite 多 phase 行
sqlite3 "R:/city_generator_base/Plugins/Monolith/Saved/ProjectIndex.db" \
  "SELECT key, value FROM meta WHERE key='schema_version';"
# 期望: schema_version=11

sqlite3 ... "SELECT asset_path, phase_id, phase_t, phase_label FROM asset_visual_geometric WHERE phase_id > 0 LIMIT 10;"
# 期望: 多行 anim/niagara 资产，phase_id ∈ {1,2}，phase_label ∈ {middle, late, peak, tail}

# 4. 用任意 anim middle-frame PNG 搜
# 找一个 anim：
sqlite3 ... "SELECT asset_path, preview_view_path FROM asset_visual_geometric WHERE phase_id=1 AND asset_path LIKE '%Anim%' LIMIT 1;"
# 用它的 preview_view_path（Saved/MonolithAssetVisual/Game.Anim/Anim_Walk_p1.png）当查询：
# MCP: asset.search_assets_by_image { "image_path": "<那条路径>", "top_k": 10 }
# 期望: 顶端第一个 hit asset_path 等于该 anim 的 asset_path，best_phase_id=1，best_phase_label="middle"
```

如果跑挂了：
- v11 migration 失败 → `WriteMeta(schema_version, 11)` 没写成 → 下次开编辑器还会从 v10 重跑，幂等
- BuildArtifact 渲染失败（anim 没 preview mesh / niagara init 失败）→ pipeline 返 Failed，该资产被标 stale，其他资产继续；不会全停
- v3 payload deserialize 失败 → MaterializeArtifact 返 false。检查 IndexerVersion=16 是否生效让 DDC miss

---

# 任务背景

UE 5.7 插件 `r:\city_generator_base\Plugins\Monolith`，用 Claude Code 在 256GB RAM / RTX 5080 / Windows 11 上做"按图搜资产"功能。已落地双 cohort 视觉索引：

- `AssetVisualGeometric`：64 维 FP32 几何特征（Hu moments + 颜色 phash + L*a*b 统计），覆盖 10 类资产
- `AssetVisualSemantic`：512 维 FP16 CLIP-ViT-B/32 ONNX 推理，DirectML / NNERuntimeORT GPU 后端

Action `asset.search_assets_by_image` 双 cohort 融合，已经能搜 StaticMesh / SkeletalMesh / Material / WidgetBlueprint / Niagara / Anim 类资产（**但只看一帧代表性 thumbnail**）。

**未完成的功能（你要做的）**：让搜索能命中**特效的某个阶段**和**动画的某个姿势** —— 也就是 multi-phase / multi-frame indexing。

---

# 已完成的工作（Phase B：工作流改造）

| 模块 | 文件 | 说明 |
|---|---|---|
| 工具栏 dropdown | `Plugins/Monolith/Source/MonolithEditor/Private/MonolithToolbar.{h,cpp}` | LevelEditor `AssetsToolBar` 上一个 `Monolith ▼` 综合菜单：构建 AssetVisual / Full Index / Incremental / 清空 |
| 子进程并行 build | `MonolithIndexSubsystem::MaterializeAssetVisualParallel(N)` | 父进程 spawn N 个 child UnrealEditor，每个跑 hash(path)%N==k 的 shard，子进程只写 DDC 不写 SQLite，父进程最后 merge |
| Live 队列持久化 | `Saved/MonolithIndex/visual_pending.txt` | AR `OnAssetsUpdatedOnDisk` callback 检测 10 个 AssetVisual 支持类，append package path；Materialize 入口读这个文件强制重建 |
| Resume + skip-list + GC | `MaterializeAssetVisualImpl` 内 | 已 materialized 的资产自动跳过；崩溃 in-flight marker 转 skip-list；每 200 个 CollectGarbage |
| 删旧 .bat | 已 `git rm` | `warmup_assetvisual*.bat` / `install_assetvisual_schedule.bat` 这条 commandlet 路 UE 5.7 commandlet 无法渲染 → 全废弃 |

工具栏入口：`Monolith ▼`
- 构建 AssetVisual：`Materialize 全部 cohort (单进程)` / `(并行 4 进程)` / `(并行 8 进程)`
- 传统 Indexer：`Full Index` / `Incremental Index`
- 清空：`清空 AssetVisualGeometric` / `清空 AssetVisualSemantic`

Console 命令同步可用：
- `Monolith.MaterializeAssetVisual` — 单进程 build + materialize
- `Monolith.MaterializeAssetVisualShard <k>:<N>` — 子进程入口
- `Monolith.MaterializeAssetVisualParallel <N>` — 父进程编排
- `Monolith.StartIndex full|incremental|auto`

当前 IndexerVersion = 14（任何渲染参数 / supported classes 改动都必须 bump 它，否则 DDC artifact identity 不变、stale artifact 仍命中）。

---

# 你的任务（Phase A）

实现"特效某阶段 / 动画某姿势"的多帧索引和搜索，让 `asset.search_assets_by_image` 能命中：

1. **VFX 阶段**：特效从 startup → peak → decay 三个时间点的形态
2. **动画姿势**：AnimSequence 在 25% / 50% / 75% 时长的姿势

---

# 真实坑点（这些是已经踩过的，请提前看）

## 1. UE 5.7 commandlet 模式无法渲染 mesh / material / widget

实测 12 轮（v3..v14 IndexerVersion）：
- SCC2D + EditorWorld → `scene_proxy=nil`，渲染全黑
- SCC2D + FPreviewScene → 同样 nil
- UThumbnailManager → 没 GUnrealEd（commandlet 是 nil）
- 即便加了 `-AllowCommandletRendering` + `MarkRenderStateDirty` + `FlushRenderingCommands` 都没用

**结论：所有渲染必须在 editor 进程跑**。子进程并行也是 N 个 UnrealEditor.exe 而不是 commandlet。

诊断方法：日志找 `RenderCanonical thumbnail diag #N: ... non_zero=...`。0 / 65536 = 渲染失败。期望值 ~60K 以上。

## 2. 编辑器进程 Materialize 是 GT 同步循环，会冻结 UI

24K 资产 × ~200ms = 80 分钟。用户已经经历过两次。子进程并行（v14 已实现）大幅缓解，4 进程理论上 20 分钟。

## 3. `UAnimSequenceThumbnailRenderer` 的 `GetTime()` 是静态函数

`UThumbnailRenderer::GetTime()` 返回 `static FGameTime` —— **不能在外部任意 SetTime**。要按时间 t 渲染 anim，要么：
- a) 自己 spawn `UAnimSingleNodeInstance` 控件 + `SetCurrentTime(t)` + `RefreshBoneTransforms()` + 自己拿 SCC2D 截
- b) 不用 thumbnail manager，直接 FPreviewScene + SkeletalMeshComponent + Anim

走 (b) 在 editor 模式有效。

## 4. `UNiagaraThumbnailRenderer` 不接受时间参数

Niagara 的 thumbnail 是固定 simulation duration。要按 phase 采样：
- 自己 spawn `UNiagaraComponent`
- `Activate()` + 手动 Tick `WorldTime` 多次（每 tick `Component->TickComponent(dt, ...)`）
- 在 t=0.5s / 1.5s / 3.0s 各 SCC2D 截一帧

注意：Niagara simulation 在 editor world 里不一定保证确定性，多次跑可能不一样。考虑用固定 seed 或文档说明。

## 5. Identity hash 不含 RenderRecipeVersion

DDC artifact 的 identity 只看 IndexerId / IndexerVersion / ArtifactSchemaVersion / DependencyVersions。**改 RenderRecipeVersion 不会让 DDC 失效**。要让 DDC stale 必须 bump `GetIndexerVersion()`。

## 6. `InsertAssetVisualEntry` 是 INSERT 不是 REPLACE

每次重跑 Materialize 都会累加新行，所以入口处加了 `ClearAssetVisualEntries(CohortName)` 全删。多 phase 改造**必须保留这个清表逻辑**，否则 `(asset_id, phase_id)` 会冲突 / 重复。

## 7. PNG cache 在 `Saved/MonolithAssetVisual/`

geometric indexer materialize 时会把 256×256 iso PNG 落盘做 preview_view。多 phase 后**每个 phase 一张 PNG**。文件命名要带 phase（如 `Game.Foo/SM_Bar_p1.png`）。

## 8. `FMaterializeAssetVisualOpts` 不能放 anonymous namespace

之前犯过：`namespace { struct ... } void Class::Method()` —— 类内 method 看不到匿名 namespace 类型。要么放 class 内（已经移到 header private 区），要么放命名 namespace。

## 9. SQLite blob binding 有引用 vs 拷贝陷阱

`SetBindingValueByIndex(idx, ptr, size, /*bCopy=*/false)` 是 SQLITE_STATIC，要求数据存活到 Execute 完成。当前代码传 false 但 Entry 是栈对象，按 const ref 跨方法传，理论 OK，但容易踩。改造时**保持 false** 不要瞎改成 true（性能差），但确保 Entry 生命周期完整。

## 10. RTX 5080 显存 16GB，并行 build 时注意

每个 child UnrealEditor 占 ~3-8GB 显存（取决于场景）。8 进程 = 24-64GB 显存，会爆。推荐 4 进程上限。多 phase 不会增加并发显存（同一 child 串行处理 phase）。

---

# 设计建议（你需要决定）

## Schema 选项 A：扩展现有表（推荐）

```sql
ALTER TABLE asset_visual_geometric ADD COLUMN phase_id INTEGER DEFAULT 0;
ALTER TABLE asset_visual_geometric ADD COLUMN phase_t REAL DEFAULT 0.0;
ALTER TABLE asset_visual_geometric ADD COLUMN phase_label TEXT DEFAULT '';
-- 同 asset_visual_semantic
```

- 单 phase 资产：phase_id=0, phase_t=0
- 多 phase 资产：phase_id=0..N-1, phase_t=对应时间
- Search 端按 asset_id GROUP BY，取最优 score

**优点**：复用现有 cohort、ANN reducer、search 路径
**缺点**：行数膨胀（24K * 3 phases ≈ 72K 行）；search 需要 dedup

## Schema 选项 B：新建 cohort `AssetVisualMultiPhase`

新建 `asset_visual_multi_phase_geometric` / `_semantic` 两张表，独立处理 Anim/Niagara。原表只放单 phase 资产。

**优点**：隔离风险，原表不动
**缺点**：search action 要查双套表 + 融合，工程量更大

**推荐 A**。

## Artifact payload schema bump

`Plugins/Monolith/Source/MonolithIndex/Private/AssetVisualArtifact.cpp`：

`PayloadSchemaVersion = 1` → `2`，新格式：
```
[uint8] schema_version = 2
[FString] asset_path
[FString] shard_id
[uint32] shard_prefix_depth
[FString] provider_id
[uint32] provider_version
[uint32] render_recipe_version
[uint32] embedding_dim
[uint8]  embedding_dtype
[uint32] num_phases  <-- NEW
for each phase:
  [uint8]  phase_id
  [float]  phase_t
  [FString] phase_label
  [TArray<uint8>] embedding_bytes
[TArray<uint8>] preview_png  <-- 还是单张代表帧
```

## Phase 定义（hardcoded per asset class）

```cpp
struct FPhaseDef { uint8 PhaseId; float PhaseT; FString Label; };

static TArray<FPhaseDef> GetPhasesForAsset(UObject* Asset)
{
    if (Cast<UAnimSequence>(Asset))
    {
        // 25/50/75% 时长
        return { {0, 0.25f, "early"}, {1, 0.50f, "middle"}, {2, 0.75f, "late"} };
    }
    if (Cast<UNiagaraSystem>(Asset))
    {
        // 0.5s / 1.5s / 3.0s 仿真后
        return { {0, 0.5f, "burst"}, {1, 1.5f, "peak"}, {2, 3.0f, "tail"} };
    }
    // 默认单 phase
    return { {0, 0.0f, ""} };
}
```

---

# TODO（按依赖顺序）

## 第 1 步：数据层

1. `Plugins/Monolith/Source/MonolithIndex/Public/AssetVisualEmbeddingProvider.h`：
   - `FIndexedAssetVisualEntry` 加 `uint8 PhaseId = 0;` / `float PhaseT = 0.0f;` / `FString PhaseLabel;`

2. `Plugins/Monolith/Source/MonolithIndex/Private/MonolithIndexDatabase.cpp`：
   - bootstrap CREATE TABLE SQL 加 3 列
   - v10→v11 migration：`ALTER TABLE` 加列；写 `schema_version=11` 到 meta 表
   - `InsertAssetVisualEntry`：bind 3 个新列
   - `GetAssetVisualEntries` / `GetAssetVisualEntryForAsset`：select 3 个新列
   - `ReplaceShadowAssetVisualEntriesForAsset`：同样
   - 注意 SQLite ALTER TABLE 不支持 default → 手动 UPDATE 旧行

3. `Plugins/Monolith/Source/MonolithIndex/Private/AssetVisualArtifact.cpp`：
   - `PayloadSchemaVersion = 2`
   - `SerializePayload(const FIndexedAssetVisualEntry&, ...)` 改签名为 `SerializePayload(const TArray<FIndexedAssetVisualEntry>&, ...)`，外加一份共享的 PreviewPng
   - `DeserializePayload` 对应改成读 num_phases + N 组数据

## 第 2 步：渲染层

4. `Plugins/Monolith/Source/MonolithCapture/Public/MonolithCaptureUtils.h`：
   - `FCanonicalRenderRequest` 加 `float PhaseT = 0.0f;` / `FName AssetClassHint;`（用于让 renderer 选择不同采样路径）

5. `Plugins/Monolith/Source/MonolithCapture/Private/AssetCanonicalRenderer.cpp`：
   - 当前 `RenderCanonical` 走 UThumbnailManager，对 Niagara / AnimSequence **新增旁路**：
     - AnimSequence：FPreviewScene + USkeletalMeshComponent + UAnimSingleNodeInstance + SetCurrentTime(PhaseT * Anim->GetPlayLength()) + RefreshBoneTransforms + SCC2D capture
     - Niagara：FPreviewScene + UNiagaraComponent + Activate + ManualTick 到 PhaseT 秒 + SCC2D capture
   - 注意：在 editor 模式 SCC2D 工作正常，只 commandlet 不行
   - 推荐：把 AnimSequence / Niagara 各做成 `RenderAnimSequencePhasePipeline` / `RenderNiagaraPhasePipeline` 独立函数

6. `RenderRecipeVersion` 4 → 5（任何 phase 采样改动都 bump）

## 第 3 步：Indexer 层

7. `Plugins/Monolith/Source/MonolithIndex/Private/Indexers/AssetVisualGeometricIndexer.cpp`：
   - `BuildArtifact`：循环 `GetPhasesForAsset(LoadedAsset)` 每个 phase 渲一次 + Encode 一次 → 收集 N 份 `FIndexedAssetVisualEntry`
   - `SerializePayload` 调新签名传整个数组
   - `MaterializeArtifact`：解 N 份 entry，循环写 SQLite N 行
   - 同样改 `MaterializeArtifactToShadow`

8. 同上改 `AssetVisualSemanticIndexer.cpp`

9. 两个 indexer header `GetIndexerVersion()` 14 → 15

## 第 4 步：搜索层

10. `Plugins/Monolith/Source/MonolithMesh/Private/MonolithAssetVisualSearchActions.cpp`：
    - 当前 result list 是 per-row。需要按 `asset_path` GROUP BY，每组取最高 score
    - 返回 JSON 时附加 `best_phase_id` / `best_phase_t` / `best_phase_label`

11. `LoadCohortShardEmbeddings` 现在每行一个 vector，多 phase 后还是一样（一个 vector 一行）。**ANN reducer 不需要改**——它本来就是 per-row。

## 第 5 步：清理 + 测试

12. v10→v11 migration 清空两张表（schema 改了 + payload schema 不兼容）
13. 重 build + 工具栏跑 Materialize 全部
14. 手动验证：
    - 选一个 anim sequence 资产（如 `/Game/Character/.../Anim_Walk`）
    - SQLite `SELECT phase_id, phase_t FROM asset_visual_geometric WHERE asset_path LIKE '%Anim_Walk%'` 应该有 3 行
    - 用 `Saved/MonolithAssetVisual/.../Anim_Walk_p1.png` 当查询输入跑 search → 顶端应该有该 anim 命中且 best_phase_id=1

---

# 具体测试用例

```sh
# 1. 跑 build
# 工具栏 → Monolith → Materialize 全部 (并行 4 进程)
# 等 ~20 分钟

# 2. 验证 SQLite 多 phase 行
"C:\Users\fangg\AppData\Local\Android\Sdk\platform-tools\sqlite3.exe" \
  "R:\city_generator_base\Plugins\Monolith\Saved\ProjectIndex.db" \
  "SELECT asset_path, phase_id, phase_t, length(embedding_bytes) FROM asset_visual_geometric WHERE phase_id > 0 LIMIT 10;"

# 3. 用任意 anim middle-frame PNG 搜
# MCP: asset.search_assets_by_image
# {
#   "image_path": "R:\\city_generator_base\\Saved\\MonolithAssetVisual\\Game.Anim\\SK_Walk_p1.png",
#   "provider": "auto",
#   "top_k": 10
# }
# 期望: 第一个 hit 是 SK_Walk 本身 + best_phase_id=1
```

---

# 风险清单

| 风险 | 可能性 | 缓解 |
|---|---|---|
| Niagara simulation 不确定 → 跨次跑 embedding 漂 | 中 | 用固定 seed；或文档说明 phase 阈值容差 |
| AnimSequence + SkelMesh 同时活在 PreviewScene → 内存涨 | 高 | 跑完一帧立刻 Unregister + GC |
| 行数膨胀 3x → SQLite 文件 222MB → ~666MB | 高 | 接受；SQLite 撑得住 |
| ANN brute-force shard 容量上限 50K → 多 phase 后超容 | 中 | 把 `FAssetVisualShardCapacityPolicy::Geometric().PerShardCap` 从 50000 提到 100000；或 shard 切更细 |
| v11 migration 失败 → 老数据丢 | 低 | 用户已经跑成功一次了，备份 `ProjectIndex.db` 再做 migration |

---

# 当前代码状态

- 编译器：MSVC 14.44.35207 / VS 2022 Enterprise
- UE 路径：`C:\Program Files\Epic Games\UE_5.7`
- 最近一次成功 build：task `bs48vjnq2`，Result: Succeeded
- 最近 git commit：仍然 `c5afe7a xxx`（Phase B 改动还没 commit，工作目录上的）
- 最近 SQLite 状态：geo 25056 行 nonzero / sem 25268 行 nonzero（Phase B 跑出来的单 phase 数据，phase_id 列不存在）
- ONNX 模型：`Plugins/Monolith/Resources/Models/clip_vit_b32_image_encoder_fp16.onnx` (167MB) 已下载

## 关键文件清单

```
Plugins/Monolith/Source/
├── MonolithCapture/
│   ├── Public/MonolithCaptureUtils.h            ← FCanonicalRenderRequest 加 PhaseT
│   └── Private/AssetCanonicalRenderer.cpp        ← 加 RenderAnimPhase / RenderNiagaraPhase
├── MonolithIndex/
│   ├── Public/
│   │   ├── AssetVisualEmbeddingProvider.h       ← FIndexedAssetVisualEntry 加 phase 字段
│   │   ├── MonolithIndexDatabase.h
│   │   └── MonolithIndexSubsystem.h
│   └── Private/
│       ├── AssetVisualArtifact.cpp              ← payload schema v1 → v2
│       ├── MonolithIndexDatabase.cpp            ← schema v10→v11 + Insert/Get 改
│       ├── MonolithIndexSubsystem.cpp           ← MaterializeAssetVisualImpl 多 phase 循环
│       └── Indexers/
│           ├── AssetVisualGeometricIndexer.{h,cpp}  ← BuildArtifact 多 phase + IndexerVersion bump
│           └── AssetVisualSemanticIndexer.{h,cpp}   ← 同上
├── MonolithMesh/
│   └── Private/MonolithAssetVisualSearchActions.cpp  ← search 端 dedup by asset_path
└── MonolithEditor/
    └── Private/MonolithToolbar.cpp               ← 工具栏入口（可能加 "重建 anim 多 phase" 按钮）
```

---

# 启动指令

新 session 第一句直接说：

> 看 `Plugins/Monolith/HANDOFF_PHASE_A.md` 接着 Phase B 做 Phase A 多 phase 索引（特效阶段 + 动画姿势）。先按 TODO 第 1 步开始数据层改造，每步编译验证再前进。任何不确定的设计决策提出来让我确认。

---

# 不要做的事

1. **不要再回 commandlet 路径渲染** — 试过 12 次都不行，浪费时间。所有渲染必须在 editor 进程
2. **不要忘记 bump IndexerVersion** — 不 bump 就算改了渲染逻辑，DDC 仍命中老数据
3. **不要在 anonymous namespace 放跨文件用的 struct** — 编译时报 "未定义类型"
4. **不要忽视 SQLite ALTER TABLE 限制** — 不支持 ADD COLUMN with default 在某些 SQLite 版本，要分两步
5. **不要直接 ship 不验证 SQLite 数据** — 走完 pipeline 一定要 sqlite3 查一下 phase_id 列实际有 0..N 不同值，不要"看起来跑过了"就交付

---

完。
