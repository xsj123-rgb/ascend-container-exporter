# 与运营平台的指标配置集成（示例）

很多 GPU/NPU 运营平台（自研或商业）都有自己的「厂商 → 层级 → 指标键 → PromQL 模板」型配置表，
用来在服务详情页渲染显存/算力曲线。昇腾容器层此前因无数据源而整层缺失，接入本探针后即可补齐。

下面以一张典型的 `gpu_metric_config(provider, layer, metric_key, promql, remark)` 表为例，
`{{.Namespace}}/{{.Pod}}/{{.Container}}/{{.UUID}}` 为平台渲染 PromQL 时的模板变量：

```sql
INSERT INTO gpu_metric_config (provider, layer, metric_key, promql, remark) VALUES
('ascend', 'container', 'memory_used',
 'sum(ascend_container_hbm_live_bytes{exported_namespace="{{.Namespace}}",pod_name="{{.Pod}}",container_name="{{.Container}}"})/1024/1024',
 '单容器真实显存已用量(MB)，需传Namespace+Pod+Container'),
('ascend', 'container', 'core_used',
 'avg(npu_chip_info_utilization{exported_namespace="{{.Namespace}}",pod_name="{{.Pod}}",container_name="{{.Container}}"})',
 '容器所在芯片算力使用率(%)，共享卡各容器同值(芯片级口径)'),
('ascend', 'container', 'pod_memory_used',
 'sum(ascend_container_hbm_live_bytes{exported_namespace="{{.Namespace}}",pod_name="{{.Pod}}"})/1024/1024',
 'Pod维度真实显存已用量汇总(MB)，只需Namespace+Pod'),
('ascend', 'container', 'pod_core_util',
 'avg by (pod_name)(npu_chip_info_utilization{exported_namespace="{{.Namespace}}",pod_name="{{.Pod}}"})',
 'Pod维度算力使用率均值(%)，只需Namespace+Pod');
```

要点：

1. **指标标签用 `pod_name`/`container_name`，不用 `pod`/`container`**
   —— 后者会被 Prometheus 抓取 target 标签（exporter 自身 pod/container）覆盖，工作负载标签全丢；
   `namespace` 冲突时会被自动重命名为 `exported_namespace`（见 README「标签设计」）。
2. 显存类指标单位统一除以 `1048576` 换算成 MB，与常见平台的 DCGM 口径对齐。
3. 算力类指标直接复用 npu-exporter 的 `npu_chip_info_utilization`
   （它对已分配芯片自带 pod/container 标签，无需 join）；本探针目前只做显存记账。
4. 若平台在启动时缓存该配置表，插入后可能需重启平台的监控相关服务。
