# ascend-container-exporter

[English](README_EN.md) | [中文](README.md)

**Per-container NPU memory monitoring for Ascend (昇腾) clusters.** When the driver exposes no
per-container data, this probe intercepts CANN runtime allocation calls via `LD_PRELOAD` and
reports **real per-Pod / per-container HBM usage** as Prometheus metrics.

> With vNPU slicing (one physical card shared by multiple containers), the official npu-exporter
> cannot provide real per-container usage: [`container_npu_used_memory` is a copy of the
> whole-card value](https://gitee.com/ascend/ascend-npu-exporter/issues/IAH8SY) (Huawei: vNPU
> monitoring needs driver-side support, "no plan yet"), and the process-level `proc-mem`
> interface is [rejected by the driver on vNPU cards](https://gitee.com/ascend/ascend-npu-exporter/issues/IAZKMT).
> This project works around the limitation entirely in **user space**.

## How it works

```
┌─ Workload container (LD_PRELOAD) ─────────────────────────┐
│ libnpu-account.so — intercepts libruntime alloc ABI:      │
│   rtMalloc / rtMallocCached / rtMemAlloc                  │
│   rtMallocPhysical / rtFree / rtsFree / rtFreePhysical    │
│ self-discovers container ID, streams JSONL events to      │
│ hostPath /run/npu-account/<container_id>.jsonl            │
└───────────────────────────────────────────────────────────┘
              │
┌─ DaemonSet: ascend-container-exporter (Go) ───────────────┐
│ • tails event files (M/F pairing + absolute snapshots)    │
│ • maps container_id → pod/namespace via K8s API           │
│ • liveness via host /proc scan (crash-proof accounting)   │
│ • GC orphan files; :9400/metrics                          │
│ • initContainer auto-distributes the shim to new nodes    │
└───────────────────────────────────────────────────────────┘
              │ ServiceMonitor → Prometheus → Grafana / your platform
```

Key design decisions (hard-won):

1. **Hook `libruntime`'s internal `rt*` ABI, not the public `aclrt*` API** — some framework
   calls bind directly via `dlsym` and bypass PLT interposition; every allocation converges
   at libruntime. (Symbol set matches what [hami-vnpu-core](https://github.com/Project-HAMi/hami-vnpu-core)
   validates in production; this is an independent, accounting-only implementation.)
2. **Label names `pod_name`/`container_name`, never `pod`/`container`** — the latter collide
   with scrape-target labels and get overwritten; `namespace` surfaces as `exported_namespace`,
   matching npu-exporter conventions for easy joins.
3. **Liveness is decided on the host side** by scanning `/proc/*/cgroup` — containers cannot
   know their host PID, and this cleanly handles crashed processes without atexit residue.

## Quick start

```bash
# 1) Build (shim on an NPU node to match its glibc; Go binaries cross-compile anywhere)
gcc -shared -fPIC -O2 -Wall -o libnpu-account.so shim/npu_shim.c -ldl -lpthread -lrt
make build    # -> bin/ascend-container-exporter + bin/ascend-mutating-webhook
docker build -f deploy/Dockerfile -t <registry>/ascend-container-exporter:v0.4.0 . && docker push ...

# 2) One-command install: certs -> Secret -> exporter DaemonSet -> auto-injection webhook
IMAGE=<registry>/ascend-container-exporter:v0.4.0 make install

# 3) Onboard workloads — zero changes to your YAML
#    The webhook patches any Pod requesting huawei.com/Ascend* resources at creation
#    (LD_PRELOAD env + /opt/npu-account RO & /run/npu-account RW mounts, and it even
#    auto-fills runtimeClassName: ascend if missing). Inspect with:
kubectl get pod -o yaml    # injected fields visible on any Ascend pod

# (Alternative) Deployed the DaemonSet without the webhook? Inject manually:
#    add the env + mounts per workload (see deploy/workload-example.yaml), or
#    python3 deploy/inject.py <deployment...>   (idempotent)

# 4) Verify
curl -s localhost:9400/metrics | grep ascend_container_hbm_live_bytes
bash test/smoke.sh <namespace> <deployment>     # real inference + metric cross-check
```

## Metrics

| Metric | Meaning |
|---|---|
| `ascend_container_hbm_live_bytes{node,namespace,pod_name,container_name}` | **current per-container HBM usage (bytes)** |
| `ascend_container_hbm_total_bytes` / `..._freed_bytes` | cumulative allocated / freed |
| `ascend_container_accounting_pids` | accounted live processes |
| `ascend_accounting_orphan_files`, `..._parse_errors_total`, `..._last_scrape_timestamp_seconds` | health |

`live` is the runtime-allocation view (≈ framework-visible memory). The gap to physical card
HBM (~4–5 GB/card in production) is fixed driver/context overhead — complementary views, not a bug.

## Validated setup

Ascend 910B3 ×16 (HAMi vNPU template slicing + whole-card), driver 25.2.0, CANN 9.0.1/9.1.0,
vllm-ascend v0.23/v0.26 + MindIE services, PyTorch 2.10 + torch_npu, k8s 1.28 (containerd),
HAMi v2.9, jemalloc coexistence. Byte-exact (256MiB request → 258MiB accounted incl. allocator
alignment); cross-checked against vLLM `gpu_memory_utilization` budgets and physical-card conservation;
production-tested on 9 services / 18 cards.

## Caveats

- `rt*` is an internal ABI: if a future CANN renames the symbols, accounting silently
  disappears — **workloads are never affected** (safe degradation). Re-run the e2e pod after CANN upgrades.
- Lazy-initializing services (e.g. OCR) report data only after the first inference request.
- Known HAMi device-plugin race: a fresh vNPU pod can lose its vNPU to the idle-cleanup loop
  within the first ~47s — just delete and recreate the pod normally; **never** `--force`-delete
  vNPU pods (leaves stale driver state); frequent pod churn can also leave the runtime reusing a destroyed vNPU ID — restarting the tenant pod on that chip rebuilds its state. Reported upstream.

## License

Apache-2.0. Symbol list informed by the open-source hami-vnpu-core project; all code independently written.
