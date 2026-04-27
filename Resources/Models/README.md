# Monolith 视觉模型

## 用途

`AssetVisualSemantic` cohort 通过 NNE + ONNX Runtime 调用此目录下的 ONNX 模型，
把 asset canonical iso view 编码成 512 维 FP16 语义向量（覆盖 StaticMesh / SkeletalMesh /
Material / WidgetBlueprint 4 类资产），用于 `asset.search_assets_by_image`
的 `provider=semantic` 召回路径。

## 当前模型

| 文件 | 用途 | 来源 | License |
|------|------|------|---------|
| `clip_vit_b32_image_encoder_fp16.onnx` | CLIP-ViT-B/32 image encoder（FP16） | OpenCLIP `ViT-B-32` checkpoint，按官方 export 流程导出 | MIT（代码） + Apache 2.0（OpenCLIP 重新训练权重） |

## 模型版本绑定

`embedding_version` 与 ONNX 文件 `Blake3` 哈希绑定。任意一次模型替换都会：

1. 让 `AssetVisualSemantic` cohort 全量 stale；
2. 触发 `MonolithIndexWarmupCommandlet -Scope=Cohort:AssetVisualSemantic` 重建；
3. 旧 DDC artifact 自然失效（identity hash 变了），由 TTL 自然淘汰。

## 替换流程

1. 把新 ONNX 文件覆写到 `clip_vit_b32_image_encoder_fp16.onnx`
2. 重启编辑器（或 `MonolithSettings` 触发 reload）
3. 验证 `Monolith.Index.AssetVisual.Semantic.ProviderHashBoundToOnnx` 自动化测试通过
4. 在编辑器 console 跑 `Monolith.MaterializeAssetVisual` 重建（**不要**跑 commandlet）

## ⚠️ AssetVisual 渲染必须在编辑器进程内跑（不能用 commandlet）

实测 UE 5.7 commandlet 模式下 `SCC2D` / `FPreviewScene` / `UThumbnailManager` 任何路径都拿不到
scene proxy（即使加了 `-AllowCommandletRendering`），渲染产出全黑帧 → embedding 全 0。

**正确流程**：
1. 启动 editor（不要用 `warmup_assetvisual.bat`，那是 commandlet 路径）
2. console 输入 `Monolith.MaterializeAssetVisual`
3. 等到 console 输出 `MaterializeAssetVisualFromCache: ... 完成: ...`

**`MaterializeAssetVisualFromCache` 内置自我保护**：
- **resume**：已经有非零 embedding 的 asset 直接跳过（中途崩了重跑不会重做）
- **崩溃 skip list**：写 `Saved/MonolithIndex/visual_render_inflight_*.txt`，崩溃后下次启动自动加入
  `Saved/MonolithIndex/visual_render_skip.txt` 永久跳过那个 asset
- **每 200 个 GC**：防 transient package 累积爆内存
- **进度日志**：每 200 个打 `进度 N/Total ... materialized=M`

## 模型加载策略

- 通过 UE 5.7 内置 `NNE` + `NNERuntimeORT` 调用，禁止直接链接 `onnxruntime` so/dll
- VRAM 上限 1.5GB；超出时 batch 自动降级到 1，PIE 期间整个 indexer 自动暂停
- ONNX 文件不存在或 NNE 不可用时，semantic provider `IsAvailable()` 返回 false，
  geometric provider 仍正常工作，`asset.search_assets_by_image` 返回 `cohort_stale=[AssetVisualSemantic]`

## 当前部署状态

模型文件由 release 流程随插件分发；首次接入项目时如果该目录为空，则 semantic cohort
自动降级到 stale-only 模式直到模型就位。
