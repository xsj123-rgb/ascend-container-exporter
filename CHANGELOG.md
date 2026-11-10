# changelog

## v0.4.0 (2026-11-10) — 一键安装 + 结构标准化

- **`make install` 一键安装**：证书/Secret/探针 DaemonSet/自动注入 webhook 全自动，
  并合并为单一镜像（内含两个二进制 + shim）；新增 `make uninstall`（`PURGE_DIRS=1` 连宿主目录清理）
- **项目结构标准化**：Go 标准 cmd/ 布局 + 根模块统一（module github.com/xsj123-rgb/ascend-container-exporter），
  清单改 `__IMAGE__`/`__CABUNDLE__` 占位符由 install.sh 渲染
- 清单更名：deploy/ascend-container-exporter.yaml → deploy/exporter-daemonset.yaml；
  webhook 清单移至 deploy/webhook.yaml；legacy setup-cert.sh 并入 install.sh
- 全新用户验收：卸载全部旧组件 → 一键安装 → 裸业务 Pod 自动注入 → HAMi 调度/显存采集全通

## v0.3.0 (2026-11-10) — MutatingWebhook 自动注入

- 新增 `webhook/` 与 `deploy/webhook/`（Deployment+Service+MutatingWebhookConfiguration+证书脚本）
- 任何请求 `huawei.com/Ascend*` 资源的 Pod 创建时自动注入三件套（LD_PRELOAD/volumeMounts/volumes），
  可选自动补 `runtimeClassName: ascend`（-add-runtime-class，默认开）
- 与 HAMi vgpu.hami.io webhook 共存：字段不相交、幂等、不碰调度领地；集群联合验证通过
  （裸 Pod → 自动注入 → vNPU 正常 → 130MB 字节级记账）
- failurePolicy=Ignore：webhook 不可用仅缺监控，绝不阻塞业务创建
- 自签证书脚本 setup-cert.sh（CA+SAN+Secret+caBundle 内联一条龙）

## v0.2.0 (2026-11-10) — 开源发布版

- 文档：完整中英文 README、平台指标配置集成示例（docs/platform-integration.md）
- 工程：Makefile、Apache-2.0 LICENSE、脱敏后的部署清单与端到端测试样例
- exporter：版本号统一为 0.2.0；行为与 v0.1.2 一致

## v0.1.2 (2026-11-09)

- **修复**：指标标签 pod/namespace/container 与 Prometheus 抓取 target 标签冲突（会被覆盖丢失），
  改为 pod_name/container_name/namespace（namespace 被自动重命名为 exported_namespace）
- 与 npu-exporter 的 container_npu_* 系列标签约定对齐，可按同名标签 join

## v0.1.1 (2026-11-09)

- 镜像底座 scratch → busybox：initContainer 可执行 cp 分发 shim（DaemonSet 自包含，
  新增 ascend 节点零手工）

## v0.1.0 (2026-11-09)

- 首个端到端可用版本
- shim：LD_PRELOAD 拦截 libruntime rt* 分配 ABI（rtMalloc/rtMallocCached/rtMemAlloc/
  rtMallocPhysical/rtFree/rtsFree/rtFreePhysical），JSONL 事件流落 hostPath
- exporter：k8s API 映射 container_id→pod、增量 tail、宿主 /proc 扫描存活判定、
  孤儿文件 GC、Prometheus :9400/metrics
- 验证：910B3 + HAMi(vNPU 模板切分) 集群，9 服务 18 卡；单容器字节级精确
  （256MiB 请求→258MiB 记账）；与 vLLM gpu_memory_utilization 预算线吻合；
  物理守恒（差异≈4-5GB/卡驱动上下文开销）
