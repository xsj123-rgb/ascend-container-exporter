# changelog

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
