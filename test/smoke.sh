#!/usr/bin/env bash
# 对已接入探针的 vLLM 系推理服务做真实推理冒烟，并回读容器级显存指标
# 用法: bash smoke.sh <namespace> <deployment-name> [exporter_ip:port]
#   exporter 地址缺省读本节点 :9400
set -euo pipefail
NS=${1:?usage: smoke.sh <namespace> <deployment> [exporter_host:port]}
DEPLOY=${2:?}
EXP=${3:-$(hostname):9400}

POD=$(kubectl -n "$NS" get pods -l app="$DEPLOY" -o jsonpath='{.items[0].metadata.name}')
IP=$(kubectl -n "$NS" get pod "$POD" -o jsonpath='{.status.podIP}')
PORT=$(kubectl -n "$NS" get pod "$POD" -o jsonpath='{.spec.containers[0].ports[0].containerPort}')
MID=$(curl -s --max-time 5 "http://$IP:$PORT/v1/models" | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"][0]["id"])')
echo "== pod=$POD model=$MID =="

echo "== 推理请求前 live =="
curl -s "http://$EXP/metrics" | grep "ascend_container_hbm_live_bytes{.*pod_name=\"$POD\"}" || echo "  (暂无序列)"

echo "== 发起 chat 推理 =="
curl -s -m 120 "http://$IP:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
  -d "{\"model\":\"$MID\",\"messages\":[{\"role\":\"user\",\"content\":\"1+1等于几？只回答数字\"}],\"max_tokens\":16}" \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  回答:", d["choices"][0]["message"]["content"][:60])'

echo "== 推理请求后 live（KV 缓存分配应反映在指标里，采集周期 10-15s） =="
sleep 16
curl -s "http://$EXP/metrics" | grep "ascend_container_hbm_live_bytes{.*pod_name=\"$POD\"" \
  | awk -v p="$POD" '{printf "  %s: %.2f GiB\n", p, $2/1073741824}'
