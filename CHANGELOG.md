# changelog

## v0.5.1 (2026-11-24) — 生产封板

- 生产验证收尾：dsv4-flash（DeepSeek-V4.1-Flash w4a8 / CANN 9.2.0 / 8×整卡）重启加载 v0.5.0 shim 后
  记账恢复——8 个 worker 进程、440.0 GB 容器级实时值
- 文档更正：实测效果表/兼容性矩阵更新至当前生产事实

## v0.5.0 (2026-11-24) — 修复 CANN 9.2.0+ 记账失效

- **背景**：CANN 9.2.0 引入版本化运行时（libruntime_v100/v200/v201 + rt2 registry），
  torch_npu/libascendcl 不再调用 rt\* 旧符号 → v0.4.x 的 rt 层钩子零命中、记账静默消失
  （业务无影响，安全降级符合设计）。生产实例：DeepSeek-V4.1-Flash w4a8 新镜像。
- **修复**：shim 增加第二层钩子——libascendcl 的 ACL vMM API
  （aclrtMalloc / aclrtMallocPhysical / aclrtFree / aclrtFreePhysical），新旧两代 CANN 通吃
- **防双计**：老栈上同笔分配会先后穿过 ACL(外层)与 rt(内层)两层——线程局部 (key,size)
  去重缓存精确匹配则内层只透传；释放方向按键查表天然幂等
- **坑**：aclrtMallocPhysical 在 9.2 为 4 参（handle,size,prop,flags），与 9.0 头文件的 3 参
  不同——签名错位表现为 error 100000 参数校验失败（预验证阶段抓到并修复）
- 双栈验证：CANN 9.2.0 记账恢复（aclrtMallocPhysical 事件流）；CANN 9.0.1 行为不变
  （rtMalloc 字节级精确、无双计、存量服务回归全过）
- 注意：shim 更新需业务 Pod 重建后生效（运行中进程持有旧 .so）

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
