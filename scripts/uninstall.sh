#!/usr/bin/env bash
# 干净卸载：webhook + 探针 + RBAC + 证书
# 用法: [PURGE_DIRS=1] bash scripts/uninstall.sh
#   PURGE_DIRS=1 额外清理各节点的 /opt/npu-account 与 /run/npu-account
set -uo pipefail
NS=${NS:-monitoring}

echo "==> 删除 webhook (MWC/Deployment/Service)"
kubectl delete mutatingwebhookconfiguration ascend-container-exporter-webhook --ignore-not-found
kubectl -n "$NS" delete deploy/ascend-mutating-webhook svc/ascend-mutating-webhook --ignore-not-found

echo "==> 删除探针 DaemonSet/RBAC"
kubectl -n "$NS" delete ds/ascend-container-exporter svc/ascend-container-exporter sa/ascend-container-exporter --ignore-not-found
kubectl delete clusterrole ascend-container-exporter clusterrolebinding ascend-container-exporter --ignore-not-found

echo "==> 删除证书 Secret"
kubectl -n "$NS" delete secret ascend-webhook-certs --ignore-not-found

if [[ "${PURGE_DIRS:-0}" == "1" ]]; then
  echo "==> 清理各 ascend 节点的宿主目录"
  for NODE in $(kubectl get nodes -l ascend=on -o jsonpath='{.items[*].metadata.name}'); do
    kubectl run "npu-account-purge-$(date +%s)" -n "$NS" --rm -i --restart=Never \
      --overrides="{\"spec\":{\"nodeName\":\"$NODE\",\"hostPID\":true,\"containers\":[{\"name\":\"purge\",\"image\":\"docker.io/library/busybox:latest\",\"command\":[\"sh\",\"-c\",\"rm -rf /host-opt/npu-account /host-run/npu-account\"],\"securityContext\":{\"privileged\":true},\"volumeMounts\":[{\"name\":\"o\",\"mountPath\":\"/host-opt\"},{\"name\":\"r\",\"mountPath\":\"/host-run\"}]}],\"volumes\":[{\"name\":\"o\",\"hostPath\":{\"path\":\"/opt\"}},{\"name\":\"r\",\"hostPath\":{\"path\":\"/run\"}}]}}" \
      --image=docker.io/library/busybox:latest >/dev/null 2>&1 \
      && echo "   $NODE: 目录已清理" || echo "   $NODE: 清理失败(手动检查)"
  done
fi

echo "==> 卸载完成（业务容器不受影响；已注入 Pod 保持现状至下次重建）"
