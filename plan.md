分析当前项目，我需要：优化index/deep index
针对特大型的项目，
1. 移除无用的/重复的/冗余的运算
2. 不阻塞主逻辑，最后自己默默后台计算
3. 多人之间运算能够共享，减少冗余计算
4. 也分析是否可以扩展Unreal的Horde Server / Zen Server 等用来减少重复运算？？如何减少？？

给我详尽的技术方案，务必深入分析清楚。我来review


---

Monolith Index / Deep Index 特大型项目优化方案
0. 现状结论（先对齐事实）
架构已有的好东西（不要推翻）：

✅ FRunnableThread + TPri_BelowNormal 后台线程
✅ SQLite schema v2 + saved_hash (Blake3) → 这是 跨用户共享的天然密钥
✅ 16ms 帧预算 + 16 资产/批 的 deep-index 节流
✅ FTS5 全文检索、64MB page cache、批量事务
✅ IMonolithIndexer::SupportsIncrementalIndex() 的脚手架（未接线）
✅ Asset Registry 回调 + 2 秒 debounce 的 live 管线（被熔断）
现状硬伤（分级）：

级别	位置	问题
🔴 P0	MonolithIndexSubsystem.cpp:396	AllAssets.Empty() — 主资产枚举被熔断，全量 index 等于空跑
🔴 P0	MonolithIndexSubsystem.cpp:1015	ValidPrefixes.Empty() — 增量 index 也被熔断
🔴 P0	MonolithIndexSubsystem.cpp:1381	RawChanges.Empty() — live 变更被吞
🔴 P0	MonolithIndexSubsystem.cpp:695/722/749/800	if (false && ...) 禁用 Dependency / Level / DataTable / Animation 5 个 post-pass
🟠 P1	同上 post-pass	FEvent::Wait() 同步等待 GameThread — 背景线程被 pin 住，编辑器无响应时这里也卡
🟠 P1	DeepIndex 路径	UObject* LoadedAsset 在 GameThread 走 LoadAsset()；每个资产冷加载耗时、占内存、触发 texture compiler
🟠 P1	MonolithIndexDatabase.cpp:257	journal_mode=DELETE + synchronous=NORMAL — 掉电安全牺牲了速度；WAL + OFF 更适合 index
🟡 P2	20 个 Indexer	各自写入独立表，没有统一 hash/lineage — 重算粒度太粗
🟡 P2	MonolithIndex ↔ MonolithSource	各自一个 SQLite 实例，两套连接、两套开关
🟡 P2	全局	无远程缓存、无 artifact 协议、无 DDC 对接
熔断为什么存在（推断）： 这些 // 临时关闭 不是 bug 是临时止血——deep indexer 加载资产 → 触发 texture compiler / shader compile / BP recompile → 几分钟到几十分钟的 stall。这就是方案要解决的根本问题。

1. 设计原则
Index ≠ Asset Load。 绝大多数信息从 FAssetRegistry + package header + tag-map 即可得到，不要 LoadAsset。
Key = (class_schema_version, indexer_version, package_saved_hash)。 这三者决定了 index 产物 内容可寻址，从而可缓存、可共享、可跨用户。
主逻辑永远不等 index。 查询接受"可能陈旧"的结果，附带一个"stale"标志；index 是最终一致的。
工作单元就是 artifact。 每个资产的 deep-index 输出是一个 blob（JSON/MsgPack/FlatBuffer），按 CAS key 存；SQLite 只存 (asset_id → artifact_key) 指针。这样才能接 Zen/Horde。
禁用的功能不是删，是重写。 所有 // 临时关闭 都是"我们知道这里会 stall"——方案必须把 stall 拆掉，再打开。
2. 计算模型重构：从 "Monolith 大循环" 到 "CAS + Queue + Worker"
2.1 引入 Content-Addressed Artifact 层
新增模块 MonolithIndexCAS：


struct FIndexArtifactKey {
    FString IndexerId;        // e.g. "Blueprint"
    uint32  IndexerVersion;   // bump when indexer logic changes
    uint32  SchemaVersion;    // bump on table layout change
    FIoHash PackageSavedHash; // from AR, Blake3
    // optional: engine version, plugin versions that materially affect output
};
// derived key = Blake3(serialize(FIndexArtifactKey))
存储后端（接口化，三个实现）：

FLocalCASStore — 项目本地 Saved/Monolith/CAS/<aa>/<bb>/<hash>.msgpack
FSharedNetworkCASStore — SMB/NAS/S3，按 key 读写
FZenCASStore — 走 Zen Server HTTP API（见 §5）
查询路径统一为：


want(packagePath) 
  → hash = AR.GetPackageSavedHash(pkg)
  → key  = derive(IndexerId, IndexerVersion, hash)
  → cas.Get(key) returns artifact OR MISS
  → if MISS enqueue Work(pkg) and return stale (DB snapshot if exists)
这一层就是"多人共享"的基础：只要两人 Blake3 相同，artifact 就 bit-identical。

2.2 Work Queue + Worker Pool
当前：全量 index 走一个 FRunnableThread，post-pass 一个一个 FEvent::Wait()。

改为 优先级工作队列 + 工作者池 + 单一调度器：


FIndexWorkItem { EPriority Pri; EWorkKind Kind; FName Package; FName IndexerId; }

Scheduler (1 thread)
  ├─ BackgroundPool (N=max(2, cores/4) threads)  ← 纯 CPU：parse、hash、SQL 写入
  ├─ GameThreadPumper                             ← 合并 GameThread 任务到 tick 预算
  └─ IOPool (2 threads)                           ← CAS get/put、网络
GameThreadPumper 的关键改造：不再用 AsyncTask + Event.Wait。而是：


// On game thread tick (in FTickableGameObject):
const double Budget = 0.004; // 4ms
const double Start = FPlatformTime::Seconds();
while (FPlatformTime::Seconds() - Start < Budget) {
    TOptional<FGameThreadJob> Job = Pumper.Dequeue();
    if (!Job) break;
    Job->Run();  // load one asset, hand back to bg thread for parse
}
好处：彻底移除 FEvent::Wait，背景线程不再 block；GT 负载可精确控制。

2.3 优先级策略
优先级	场景
P0 Interactive	用户当前选中、打开、引用的资产（来自 FAssetEditor、FContentBrowser 选中事件）
P1 Live	AR 回调变更（debounce 后）
P2 Warmup	启动时高价值资产（Levels、主 Blueprints）
P3 Background	全量 catchup
Interactive 绕过队列直接入池首位，LRU 5s 窗口内重复需求自动合并。

3. 消除"无用/重复/冗余"计算
3.1 去掉重复加载
现状问题：每个 deep indexer 独立调用 LoadAsset；同一个 Blueprint 在 BlueprintIndexer + DependencyIndexer + GASIndexer（如果是 GA）加载 3 次。

方案：FAssetLoadBroker

一次 LoadPackageAsync → 所有对该 package 感兴趣的 indexer 回调里串行跑
加载完毕 1 秒内 FGCObjectScopeGuard 持有；窗口期复用，1 秒后释放让 GC 回收
命中 broker 的调用不再入 GT pumper
3.2 避免不必要的 LoadAsset
90% 信息其实不需要加载 UObject：

信息	当前做法	更好做法
Blueprint 父类	LoadAsset → Cast<UBlueprint>	AR tag ParentClass
Blueprint 接口	Load	AR tag ImplementedInterfaces
Texture 尺寸	Load	AR tag Dimensions / SizeX/Y
Mesh 三角形数	Load	AR tag + FAssetTagsAndValues
GameplayTag 引用	Load	AR searchable reference（AssetRegistry export）
Material 参数名	Load	AR searchable reference
资产依赖	仍需 AR	GetDependencies — 无需 load
行动：给每个 indexer 加 EIndexMode { TagOnly, NeedsLoad }；TagOnly 的完全不走 GameThread。预估能把 60-70% 的 indexer 调用从 GT 搬到后台线程。

3.3 Indexer 幂等化（去重引擎）
IMonolithIndexer 加三个虚函数：


virtual uint32 GetIndexerVersion() const = 0;    // bump on logic change
virtual EIndexMode GetMode() const { return EIndexMode::TagOnly; }
virtual void Produce(FIndexContext&, FIndexArtifact& Out) = 0;  // pure: in=pkgHash+tags+(uobject), out=blob
Produce 必须是纯函数：输入相同输出必须相同。
把 InsertDependency / InsertNode 等"写库"动作从 indexer 里移除——indexer 只产 blob；DB 落库是调度器的事（blob → rows 的固定翻译）。

好处：

同内容哈希的资产只跑一次 Produce（进程内 memoize）
blob 可缓存到 CAS → 跨用户共享
单测可 property-test（同输入同输出）
3.4 批量化 AR 查询
DependencyIndexer 现在是 post-pass 一次性扫描全部资产。改为：

主循环内 AR.GetDependencies(pkg) 顺带产出 edges blob
删除 post-pass（那个 if (false && DepIndexer) 直接删）
同理 LevelIndexer 对 streaming levels 也可用 AR 替代加载（LevelInstance、WP grids 都在 tag 里）。

3.5 Schema 版本化 & 部分失效
当前 schema_version=2 是整库级别。需要 per-indexer schema version：


CREATE TABLE indexer_versions (
    indexer_id TEXT PRIMARY KEY,
    schema_version INTEGER,
    code_version INTEGER
);
升级 BlueprintIndexer 时 只重跑 Blueprint，不动其他表。省 90% 重建时间。

4. 主逻辑不阻塞 —— 启动期 & 查询期双改
4.1 启动（关键体感）
现状：editor 启动 → subsystem Initialize → 立刻起全量 index 线程。大型项目冷启动到可用中间 15-60s。

改造：

Initialize 里只做：打开 DB、加载 indexer registry、注册 AR 回调、注册 tickable。不启动任何 index。 目标 < 50ms。
PostEngineInit 延后 2 秒：
读 indexer_versions 表 + DB schema_version
如版本一致 & DB 非空 → 不做任何事，进入 live 模式
否则 → 启动 worker pool，入队低优先级 catchup
首次 MCP 查询进来 → 查询层从 DB 返回当前快照 + {"stale": true, "progress": 0.3}，永不阻塞
AR OnFilesLoaded 触发后再算 delta，不要提前
效果：editor 本体秒开，index 在后台安静进行，查询随时可用。

4.2 Live 更新重写（去除熔断）
OnAssetsUpdatedOnDisk → lock-free SPSC ring 入队 → 2s debounce tick → 每包生成 FIndexWorkItem(Pri=Live) 入池。

关键：不再走 "先删后插" 模式，而是"写入新 artifact，原子替换 DB 指针"。读者并发查询不会看到"半删"状态。


// atomic swap via single UPDATE
UPDATE assets SET artifact_key = ? WHERE path = ?;
并 取消 RawChanges.Empty() 熔断，但 live 路径走 CAS hit-first：如果用户只是 resave 没有实质修改，saved_hash 没变 → CAS 命中 → 零工作。

4.3 UI 反馈
启动期不再弹 FAsyncTaskNotification（除非 > 30s）
状态条改成状态栏 badge，点开才见详情
查询接口返回 stale_paths 子集让 LLM 可自己决定是否等待
5. 多人共享计算 —— 本地团队 → 大规模
5.1 Tier 1：团队内网共享 CAS（2 天工作量，收益最大）
设计：

一台 NAS / 任意 SMB 共享：\\team-nas\UE_IndexCache\<project_guid>\<aa>\<bb>\<key>.artifact
客户端 FSharedNetworkCASStore 实现 Get/Put/Exists：
Exists → FPaths::FileExists
Get → 拷贝到本地临时，校验 Blake3，反序列化
Put → 写临时文件 + rename（POSIX atomic）
共享条件：同 (IndexerId, IndexerVersion, PackageSavedHash) → 全团队只算一次。因为 Blake3 由 UE 对 package 的规范化序列化得出，不包含绝对路径、不包含时间戳（UE 保证 DeterministicCook 条件下稳定），所以 A 保存的 BP_Player，B 拉到后 hash 相同。

注意：

开发中的 dirty package 不会被 AR 写 hash，自然不上传；commit 后才 hash 稳定
需要给 CAS 加 GC：atime > 30d 删除
写入用 advisory lock 避免两人同时写同 key（冲突概率极低，但 rename-atomic 已经够了）
配置：项目设置加 SharedCacheRoot: FDirectoryPath，空则走纯本地。

5.2 Tier 2：HTTP Cache Server（小型自建，1-2 周）
当 NAS 延迟/权限不够时，起一个轻量 HTTP 服务：


GET  /cache/{key} → 200 artifact | 404
PUT  /cache/{key} → 201 (if absent, CAS semantics)
HEAD /cache/{key} → existence probe
参数化 FHttpCASStore；加 X-Project-Id、Authorization。后端存 S3/R2/MinIO 皆可，服务无状态。

流量预估（50 人团队，20w 资产）：

首次全仓填充：单人 ~3-5 GB index artifacts
每人日均 <100 MB（只传本地改过的）
hit ratio 长期 > 95%
5.3 Tier 3：对接 Unreal Zen Server（推荐 - 复用成熟设施）
Zen 是 Epic 官方的 高性能内容寻址 HTTP 服务，原本给 DDC 用，但它本质就是个 CAS blob store + namespaced buckets：


PUT /ns/{namespace}/blobs/{ioHash}   # raw blob
GET /ns/{namespace}/blobs/{ioHash}
POST /ns/{namespace}/refs/{bucket}/{key}  # named reference
做法：

在 zen.conf 里注册新 namespace monolith.index
FZenCASStore 调用 ZenHttp::Request()（引擎已有 ZenHttp.h）
artifact = raw blob（put/get by Blake3），asset_path → artifact = ref
团队里原本跑 Zen 当 DDC 用 → 几乎零运维增量
为什么值得：

Zen 已经处理好了 重复数据删除、原子写、pull-through 代理、多级层级
Zen 原生支持 多机复制（cascade），可接中心 NAS 或云对象存储
用户本来就得装 Zen（UE 5.4+ 趋势），不增加依赖
相同 engine-version 下多项目共享基础设施
限制：Zen ref 的 TTL 策略需要监控，一些 bucket 默认 14 天——可以配长期 bucket。

5.4 Tier 4：Horde Server 远程执行（大规模共享 + 远端计算）
Zen 解决 "存"，Horde 解决 "算"。Horde 是 Epic 的分布式作业服务器（BuildGraph 的大脑），能把计算任务扔到 farm 上跑。

对接思路：

写一个 Horde Job 模板 MonolithIndexJob.bgscript：输入 (package_list, engine_version, plugins_manifest)，输出一批 artifact 到 Zen
客户端缺 index 时：
本机优先（命中 Zen 就不必算）
批量 miss 超过阈值（比如 > 500 个资产）→ 提交 Horde job，客户端 polling
Horde agent 可以开在：
夜间空闲的美术/程序机器
专用 farm
CI runner
CI 集成：在 Perforce/Git post-commit hook 里触发 monolith-index warmup job，submit 的所有变更 15 分钟内全团队 已经算好了
关键文件参考：

Engine/Source/Programs/Horde/HordeAgent/*
UE 5.7 Engine/Source/Developer/Zen/*
落地顺序建议：Tier 1（一周内落地）→ Tier 3（Zen，两周）→ Tier 4（Horde，按需，可能一个月）。Tier 2 只在无法部署 Zen 时作为替代。

5.5 安全 & 信任模型
Artifact 是只读、可验证的（下载后重算 Blake3）
恶意 artifact 注入不是主攻面，但加：
可选签名：(key, blob, HMAC(team_secret))
Namespace per project（不跨项目共享，避免 class hash 冲突）
6. 存储层优化（SQLite 本身）
切 WAL + memory temp：

PRAGMA journal_mode=WAL;          -- 并发读不阻塞写
PRAGMA synchronous=NORMAL;        -- 保持
PRAGMA temp_store=MEMORY;
PRAGMA mmap_size=2147483648;      -- 2GB mmap
PRAGMA cache_size=-131072;        -- 128MB
Asset 表加 artifact_key 列，原 nodes/connections/... 表改为 view over blob（按需解 blob） OR 保留为热查询缓存但可随时重建（从 CAS blob 物化）
按月/项目分库（可选）：index_main.db + index_deep_<indexerid>.db，用 ATTACH 联查。benefit：重置单个 indexer 不锁主库
FTS5 用 external-content table：当前应是内嵌的，改成 external-content 避免内容双写
7. 查询 API 的无阻塞改造
MCP action 调用举例 query_blueprint_nodes(path)：


FIndexQueryResult Result = Index->Query(...);
if (Result.IsStale()) {
    // option A: return stale data + progress hint
    //   → LLM can choose to retry later
    // option B: if caller passed wait_ms>0, block up to that
    //   → used by interactive editor UI
}
默认 stale-ok；MCP action 的参数加 wait_ms（默认 0）
Interactive path 可用 PrioritizeAsset(path) 把 key 提到 P0
新增 get_index_state() action：返回 {total, indexed, pending, cas_hit_ratio, last_error}
8. 可观测性（必不可少）
现状没有 metrics，调优盲飞。加：

Per-indexer 计时：Indexer → { calls, total_ms, p50/p95/p99, load_ms, produce_ms, serialize_ms }
CAS 命中率：{local_hit, shared_hit, miss, put_bytes, get_bytes}
Queue 水位：{pending_interactive, pending_live, pending_bg}
GT 预算占用：GameThreadPumper 每秒消耗 ms 数
暴露为 MCP action index_metrics() + 可选 Prometheus 文本格式（/metrics 端点，复用现有 9316 HTTP）
9. 落地顺序与验收
阶段 A：去熔断 + 去阻塞（1-2 周，最大收益）
删除 10 处 // 临时关闭 前，先做好 GameThreadPumper、AssetLoadBroker、batch AR-tag fast-path
Dependency / Level / DataTable / Animation post-pass 重写为流式、取消 FEvent::Wait
Indexer 加 GetMode()，TagOnly 的走纯 BG 线程
验收：10w 资产项目冷启动后 index，编辑器 UI 帧率全程 > 30fps、启动到可查询 < 3s
阶段 B：CAS 抽象 + 本地 CAS + 版本化（1 周）
IIndexCASStore 接口 + FLocalCASStore
IMonolithIndexer::Produce 纯函数化
indexer_versions 表 + 部分失效
验收：改一个 indexer 代码只重跑该 indexer；进程内 memoize 可见 hit
阶段 C：共享 CAS Tier 1（1 周）
FSharedNetworkCASStore（SMB/NAS）
项目设置 UI、metrics
验收：两人同仓拉同一 commit，第二人 index 时间 < 第一人的 20%
阶段 D：Zen 对接（2 周）
FZenCASStore 走 ZenHttp
运维脚本、namespace 配置
验收：替换 Tier 1 后命中率保持、延迟 < 50ms p95
阶段 E（可选）：Horde 远端 warmup（2-4 周）
BuildGraph job 模板
Post-commit hook 触发
验收：新 commit 15 分钟内全团队命中 Zen
10. 风险 & 反向权衡
风险	说明	对策
Blake3 saved_hash 不稳定	UE 的 package hash 会因 dirty cook/build id 漂移	先做 A/B：同一人两次保存相同内容的 hash 比对；不稳就 fallback 到 sha1(serialized tags)
非 deterministic indexer	指针/GUID/迭代顺序导致产物不同	Produce 必须排序输出、禁 ptr、禁未排序 map
GT pumper 抖动	4ms 预算偶发不足	自适应：根据过去 1s 实际 frame time 动态 2-8ms
Zen 不可用/超时	网络故障退化	所有 remote 调用带 250ms 超时 → fallback local compute
CAS 体积膨胀	artifact 文件多	压缩（zstd）、GC by atime、可选按类聚合成 pack 文件
禁用代码本来就有 bug	开回来才发现	每解熔断一个 indexer 单独灰度 + 只读 shadow 模式对比
11. 关键代码改动点清单（精确定位）
MonolithIndexer.h — 加 GetMode / GetIndexerVersion / Produce
MonolithIndexSubsystem.cpp:396 — 删 AllAssets.Empty() + 走新调度器
MonolithIndexSubsystem.cpp:695-900 — 5 处 post-pass 全部重写为 streaming
MonolithIndexSubsystem.cpp:1015 / :1381 — 开回 live 管线
MonolithIndexDatabase.cpp:257 — PRAGMA 更新
新增 Source/MonolithIndexCAS/ 模块（IIndexCASStore、FLocalCASStore、FSharedNetworkCASStore、FZenCASStore）
新增 FIndexScheduler + FGameThreadPumper（替代 FIndexingTask 内的 AsyncTask+Event.Wait 模式）
MonolithSettings.h — 加 SharedCacheMode / SharedCacheRoot / ZenNamespace / EnableHordeWarmup
请 review 上述方案。重点请反馈：

Blake3 saved_hash 在你们 P4/Git 工作流下是否稳定 —— 这是共享层的根基
团队是否已部署 Zen —— 决定走 Tier 1 NAS 还是直接 Tier 3
Horde 可用性 —— 团队是否已有 Horde CI farm
// 临时关闭 当初熔断时的痛点记录 —— 有没有 profiling 数据，我可以据此调整 GT pumper 预算
正确性 vs 延迟 —— 查询默认 stale-ok 对你们的 MCP action 消费者（LLM）是否可接受