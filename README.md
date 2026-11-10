# ascend-container-exporter

[中文](README.md) | [English](README_EN.md)

**昇腾 NPU 容器级显存监控探针** —— 在驱动不提供容器粒度数据的前提下，通过 `LD_PRELOAD` 拦截 CANN 运行时分配接口，实现**每个 Pod / 每个容器的真实显存使用量**采集，输出 Prometheus 指标。

> 在 vNPU 切分（一张物理卡切给多个容器）场景下，华为官方 npu-exporter 无法提供容器级真实用量：
> [`container_npu_used_memory` 实为物理卡整卡值复制](https://gitee.com/ascend/ascend-npu-exporter/issues/IAH8SY)（官方回复：vNPU 监控需驱动侧接口支持，暂无计划）；
> 进程级接口 `proc-mem` 在 vNPU 卡上[被驱动直接拒绝](https://gitee.com/ascend/ascend-npu-exporter/issues/IAZKMT)。
> 本项目绕开驱动限制，在**用户态**完成容器级记账。

---

## 实测效果

生产集群（Ascend 910B3 × 16 卡，HAMi vNPU 模板切分 + 整卡混部，9 个 vLLM/MindIE 系服务）：

| 服务 | 卡型 | 容器级显存（本探针实测） | 交叉验证 |
|---|---|---|---|
| dsv4-flash | 8×整卡 | 431.4 GB | vLLM 预算 0.9×64G×8 = 460.8G，加载期 464G → 稳态 431G（图捕获缓存释放）✓ |
| qwen36-35b | 4×整卡 | 222.9 GB | vLLM 预算 0.9×64G×4 = 230.4G ✓ |
| qwen3-asr | vNPU 32G 切分 | 25.55 GB | vLLM 预算 0.9×30G ✓；与同卡另一容器（tts 8.84G）**各报各的** |
| qwen3-tts / tts-clone | vNPU 32G 切分 | 各 8.5~8.9 GB | 同模型同配置双值一致；推理请求后实时 +0.3G（KV 缓存）✓ |
| mineru（懒加载 OCR） | vNPU 32G 切分 | 2.31 GB | 首个业务请求后自动出数 ✓ |

**精度**：256MiB 分配请求 → 记账 270532608 字节（258MiB，含分配器对齐），字节级精确。
**守恒**：同一节点 Σ容器记账 + ~4-5GB/卡（驱动/上下文固定开销，不经过 CANN 分配接口）≈ 物理卡 HBM 实测值。

## 工作原理

```
┌─ 业务容器（LD_PRELOAD 注入）───────────────────────────┐
│  libnpu-account.so（C，~300 行）                        │
│   拦截 libruntime 分配 ABI:                              │
│    rtMalloc / rtMallocCached / rtMemAlloc               │
│    rtMallocPhysical / rtFree / rtsFree / rtFreePhysical │
│   自发现容器 ID(/proc/self/cgroup)                       │
│   事件流 JSONL → hostPath /run/npu-account/<cid>.jsonl  │
└──────────────── hostPath 共享 ──────────────────────────┘
              │
┌─ DaemonSet: ascend-container-exporter（Go，单文件）─────┐
│ ① tail 事件文件（M/F 配对 + S 快照，幂等重放）            │
│ ② K8s API: container_id → pod/namespace/container       │
│ ③ 宿主 /proc 扫描(cid+NSpid) 做进程存活判定，剔除崩溃残留 │
│ ④ 孤儿文件 GC（容器已删，超阈值清理）                     │
│ ⑤ :9400/metrics + initContainer 自动向新节点分发 shim    │
└─────────────────────────────────────────────────────────┘
              │ ServiceMonitor → Prometheus → Grafana / 运营平台
```

### 三个关键设计决策（踩坑实录）

1. **拦 `libruntime` 的 `rt*` 内部 ABI，而不是 `libascendcl` 的 `aclrt*` 公共 API**
   实测（LD_DEBUG=bindings）部分框架调用经 `dlsym` 直连真函数、绕过 PLT，ACL 层拦截会漏；
   而一切显存分配最终收敛到 libruntime 的 `rtMalloc` 族——那是唯一咽喉点。
   （符号集与 [hami-vnpu-core](https://github.com/Project-HAMi/hami-vnpu-core) 生产验证一致，本项目独立实现了纯记账子集。）
2. **指标标签用 `pod_name`/`container_name`，绝不用 `pod`/`container`**
   后者与 Prometheus 抓取 target 标签冲突会被直接覆盖（工作负载标签全丢）；
   `namespace` 冲突时被自动改名为 `exported_namespace`——与 npu-exporter 的既有约定一致，可按同名标签 join。
3. **进程存活判定在宿主机侧做**
   容器内拿不到自己的宿主 pid（`/proc/self/sched` 返回的也是 namespace pid）；
   聚合器扫宿主 `/proc/*/cgroup` 的 `cri-containerd-<cid>` + `NSpid` 末位，天然解决
   「进程崩溃未走 atexit 导致记账残留」问题。

## 快速开始

### 第 1 步：构建

```bash
# shim —— 在 NPU 节点上编译（保证 glibc 不新于目标节点；离线环境推荐此法）
gcc -shared -fPIC -O2 -Wall -o libnpu-account.so shim/npu_shim.c -ldl -lpthread -lrt

# exporter —— 任意开发机交叉编译（linux/arm64 静态二进制）
cd exporter && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o ../ascend-container-exporter . && cd ..

# 镜像 —— busybox 底（内网/离线友好）
docker build -f deploy/Dockerfile -t <你的仓库>/ascend-container-exporter:v0.2.0 .
docker push <你的仓库>/ascend-container-exporter:v0.2.0
```

有外网的环境也可用 `deploy/Dockerfile.multistage` 一键构建（注意其中关于 glibc 兼容的说明）。

### 第 2 步：一键安装（探针 DaemonSet + 自动注入 webhook）

```bash
make shim build image IMAGE=<你的仓库>/ascend-container-exporter:v0.4.0   # shim 须在 NPU 节点编译
IMAGE=<你的仓库>/ascend-container-exporter:v0.4.0 make install
```

`make install` 一条龙完成：证书生成 → Secret → 探针 DaemonSet（自动铺 shim 到每个 NPU 节点）→
自动注入 webhook。装完即"业务下发即被监控"。卸载：`make uninstall`（`PURGE_DIRS=1` 连宿主目录一起清）。

不想要 webhook 自动注入的，也可以只部署探针：`kubectl apply -f deploy/exporter-daemonset.yaml`。

### 第 3 步：工作负载接入（业务侧，比平时多 3 行）

给 Deployment 增加三样东西（完整示例见 `deploy/workload-example.yaml`）：

```yaml
runtimeClassName: ascend                      # 昇腾 Pod 本来就需要
env:
- name: LD_PRELOAD
  value: /opt/npu-account/libnpu-account.so   # 镜像自带 LD_PRELOAD 时用冒号追加，不可覆盖
volumeMounts:
- {name: npu-account-lib,   mountPath: /opt/npu-account, readOnly: true}
- {name: npu-account-data,  mountPath: /run/npu-account}   # 必须可写！
volumes:
- {name: npu-account-lib,  hostPath: {path: /opt/npu-account, type: Directory}}
- {name: npu-account-data, hostPath: {path: /run/npu-account, type: DirectoryOrCreate}}
```

存量服务批量注入（幂等，自动处理 jemalloc 等已有 LD_PRELOAD 与空 volumeMounts）：

```bash
python3 deploy/inject.py <deployment-1> <deployment-2> ...
```

### 第 3.5 步：MutatingWebhook 自动注入（`make install` 已内置）

`make install` 会一并部署自动注入 webhook：**任何请求 `huawei.com/Ascend*` 资源的 Pod 创建时被自动补齐三件套**，你的下发方式完全不变（甚至忘写 `runtimeClassName` 都会自动补 `ascend`）。

设计要点：

- **与 HAMi 的 vgpu.hami.io webhook 共存**：只补 env/volumeMounts/volumes 三处，与 HAMi 的
  annotation/调度链字段不相交；不碰 nodeName/schedulerName/resources；幂等可重入
- **只动该动的**：非昇腾 Pod 原样放行；`failurePolicy: Ignore` —— webhook 挂了业务照常创建，仅缺监控
- `runtimeClassName: ascend` 可选自动补齐（`-add-runtime-class=true` 默认开启，仅当 Pod 未写时）
- 建议同时部署，服务数量多后可退役手动 inject 流程

滚动重启后即生效：服务加载模型时显存逐笔入账，`ascend_container_hbm_live_bytes` 开始出数。

### 第 4 步：验证

```bash
# 任一 NPU 节点：
curl -s localhost:9400/metrics | grep ascend_container_hbm_live_bytes

# 对 vLLM 系服务做真实推理并观察指标随 KV 缓存分配波动：
bash test/smoke.sh <namespace> <deployment>
```

端到端自检 Pod（验证 vNPU 分配 + 注入 + 记账 + 指标全链路）：`test/e2e-test-pod.yaml`。

## 指标参考

| 指标 | 说明 |
|---|---|
| `ascend_container_hbm_live_bytes{node,namespace,pod_name,container_name}` | **容器当前显存占用（字节）**，= Σ 存活进程的活跃分配 |
| `ascend_container_hbm_total_bytes{...}` | 容器累计分配（字节） |
| `ascend_container_hbm_freed_bytes{...}` | 容器累计释放（字节） |
| `ascend_container_accounting_pids{...}` | 该容器当前有记账的进程数 |
| `ascend_accounting_orphan_files` | 容器已不存在、待 GC 的记账文件数 |
| `ascend_accounting_parse_errors_total` | 累计解析失败行数（应为 0 或极低） |
| `ascend_accounting_last_scrape_timestamp_seconds` | 最近一次采集的 Unix 时间 |

> Prometheus 会在抓取时附加 target 标签，最终序列中工作负载命名空间显示为 `exported_namespace`。

**口径说明**：`live` 是「CANN 运行时向驱动申请的设备内存」视角 ≈ 框架视角的显存占用；
与物理卡 HBM 的差值（实测 4-5GB/卡）是驱动/上下文固定开销，二者口径互补、不矛盾。

## 配置参考

**shim（业务容器内，环境变量）**

| 变量 | 默认 | 说明 |
|---|---|---|
| `NPU_ACCOUNT_DIR` | `/run/npu-account` | 事件流目录（通常由挂载决定，无需设置） |
| `NPU_ACCOUNT_ID` | 自动发现 | 覆盖容器 ID（一般不用） |

**exporter（DaemonSet 参数）**

| 参数 | 默认 | 说明 |
|---|---|---|
| `-dir` | `/run/npu-account` | 事件流目录 |
| `-listen` | `:9400` | HTTP 监听 |
| `-node` | `$NODE_NAME` | 节点名标签 |
| `-interval` | `10s` | 采集周期 |
| `-gc-after` | `30m` | 孤儿文件清理阈值 |
| `-kubeconfig` | in-cluster | 调试用 |

**事件流格式**（`/run/npu-account/<container_id>.jsonl`，供自定义聚合器复用）：

```jsonl
{"t":"I","pid":83,"cid":"..."}                                  # 进程加载 shim
{"t":"M","pid":83,"sym":"rtMalloc","key":"0x12c1000c3000","size":270532608}
{"t":"S","pid":83,"hpid":83,"live":270532608,"total":29301540608,"freed":1929379840}
{"t":"F","pid":83,"sym":"rtFree","key":"0x12c1000c3000","size":270532608}
{"t":"X","pid":83,"hpid":83,"live":0,"total":29301540640,"freed":29301540640}
```

`S` 为每事件后的**绝对值快照**，聚合器只需取每 pid 最后一条即可，天然幂等。

## 兼容性

| 组件 | 实测版本 |
|---|---|
| 硬件 | Ascend 910B3（aarch64） |
| 驱动/固件 | npu-smi 25.2.0 |
| CANN | 8.x 符号集（参考 hami-vnpu-core）；9.0.1 / 9.1.0 实测通过 |
| 推理栈 | vllm-ascend v0.23 / v0.26、MindIE 系服务、PyTorch 2.10 + torch_npu |
| Kubernetes | v1.28（containerd；需 Pod 的 cgroup 含 `cri-containerd-<id>.scope`） |
| 调度 | HAMi v2.9（ascend-device-plugin 模板切分模式）+ 整卡混部 |
| 共存 | jemalloc LD_PRELOAD（符号正交，冒号拼接） |

未实测但理论上可用：910B 系列其它型号 / 310P 等（只要 libruntime 导出同名 rt* 符号）；
MindIE 大版本更新后建议先用 `test/e2e-test-pod.yaml` 回归一次。

## 已知限制与注意事项

1. **rt\* 是 libruntime 内部 ABI**：华为不承诺跨版本稳定。若未来 CANN 改名/移除符号，
   shim 的导出函数不再被调用 → 记账静默消失（**不会影响业务分配**，安全降级），升级 CANN 后请回归验证。
2. **性能开销极小但存在**：每次显存分配/释放多一次 mutex + 一次小 JSON 行追加（<1μs 量级）；
   模型加载期数千次分配实测无感。
3. **懒加载服务首数据延迟**：CANN 运行时在首次使用 NPU 时才加载，记账随首个请求出现（如 OCR 类服务）——属预期。
4. **HAMi vNPU 清理循环竞态（HAMi 侧问题）**：新 Pod 建立后 ~47s 内若被插件的闲置清理命中，vNPU 被误销毁，
   Pod 永久带着失效设备（`aclInit` 报 507008 / `rtGetDevMsg` 不支持）。规避：失败 Pod 正常 `kubectl delete` 重建；
   **千万不要 `--force --grace-period=0` 强删 vNPU Pod**（会留驱动脏状态）。建议向 HAMi 社区反馈修复。
5. **单副本 Deployment 滚动可能卡 Pending**：maxSurge 新 Pod 抢不到记账额度时，删旧 Pod 放行；
   若旧 ReplicaSet 自愈拉起竞争 Pod，先 `kubectl scale rs <旧RS> --replicas=0`。
6. 显存**超卖/共享上限**类指标请继续使用调度器（如 HAMi）的分配口径；本探针提供的是**真实用量**，两者互补。

## 排障 FAQ

| 症状 | 排查 |
|---|---|
| `/run/npu-account/` 里没有容器对应文件 | ① 挂载是否可写（readOnly 会导致降级写容器 /tmp）② LD_PRELOAD 是否被镜像 entrypoint 覆盖 |
| 文件在但指标为 0 | 服务懒加载未触达 NPU（发一个推理请求）；或进程刚退出（X 行/liveness 剔除，属正确语义） |
| `aclInit` 失败 / `rtGetDevMsg` 不支持 | 见「已知限制」第 4 条：vNPU 竞态或强删残留，正常删除 Pod 重建 |
| 指标里 pod/namespace 标签不对 | 确认用的是 `pod_name`/`exported_namespace`（README「标签设计」） |
| CANN 升级后数据消失 | rt* 符号变更，在节点上 `nm -D /usr/local/Ascend/*/lib64/libruntime.so \| grep rtMalloc` 核对 |

## 路线图

- [x] ~~MutatingWebhook 自动注入~~ → v0.3.0 已实现（见「第 3.5 步」）
- [ ] 容器级算力使用率（`rtKernelLaunch` 族拦截 + 时间片统计）
- [ ] recording rule / Grafana dashboard 开箱样例
- [ ] 310P / 910C 等型号适配验证
- [ ] Prometheus histogram（分配大小分布）

## 致谢

- [Project-HAMi/hami-vnpu-core](https://github.com/Project-HAMi/hami-vnpu-core)——拦截符号集与「libruntime 是咽喉点」的实践佐证
- [ascend/npu-exporter](https://gitee.com/ascend/ascend-npu-exporter)——芯片级指标与其容器模式的前期探索
- 所有在 Gitee issue 里报告「容器级采不到」的同路人

## License

Apache-2.0。本项目的实现参考了 hami-vnpu-core 公开源码中公开的符号清单，全部代码独立实现。
