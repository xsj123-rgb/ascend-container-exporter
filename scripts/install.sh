#!/usr/bin/env bash
# 一键安装：证书 → Secret → DaemonSet(探针) + Deployment(webhook)
# 用法: IMAGE=<镜像> [NS=monitoring] bash scripts/install.sh
set -euo pipefail
NS=${NS:-monitoring}
IMAGE=${IMAGE:?用法: IMAGE=<你的镜像> bash scripts/install.sh}
DIR=$(cd "$(dirname "$0")/.." && pwd)
SVC=ascend-mutating-webhook
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "==> [1/4] 生成 webhook 证书"
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout "$WORK/ca.key" -out "$WORK/ca.crt" -subj "/CN=ascend-mutating-webhook-ca" >/dev/null 2>&1
cat > "$WORK/server.cnf" <<EOF
[req]
distinguished_name = dn
req_extensions = ext
prompt = no
[dn]
CN = $SVC.$NS.svc
[ext]
subjectAltName = DNS:$SVC, DNS:$SVC.$NS, DNS:$SVC.$NS.svc
EOF
openssl req -newkey rsa:2048 -sha256 -nodes \
  -keyout "$WORK/tls.key" -out "$WORK/tls.csr" -config "$WORK/server.cnf" >/dev/null 2>&1
openssl x509 -req -in "$WORK/tls.csr" -CA "$WORK/ca.crt" -CAkey "$WORK/ca.key" \
  -CAcreateserial -days 3650 -sha256 \
  -extfile "$WORK/server.cnf" -extensions ext -out "$WORK/tls.crt" >/dev/null 2>&1

echo "==> [2/4] 写入证书 Secret"
kubectl -n "$NS" create secret tls ascend-webhook-certs \
  --cert="$WORK/tls.crt" --key="$WORK/tls.key" --dry-run=client -o yaml | kubectl apply -f -

echo "==> [3/4] 部署 DaemonSet(探针) + Webhook"
CABUNDLE=$(base64 < "$WORK/ca.crt" | tr -d '\n')
sed -e "s|__IMAGE__|$IMAGE|g" "$DIR/deploy/exporter-daemonset.yaml" | kubectl apply -f -
sed -e "s|__IMAGE__|$IMAGE|g" -e "s|__CABUNDLE__|$CABUNDLE|g" "$DIR/deploy/webhook.yaml" | kubectl apply -f -

echo "==> [4/4] 等待就绪"
kubectl -n "$NS" rollout status ds/ascend-container-exporter --timeout=180s
kubectl -n "$NS" rollout status deploy/ascend-mutating-webhook --timeout=120s

cat <<TIP

安装完成。自检：
  kubectl -n $NS get pods -l 'app in (ascend-container-exporter, ascend-mutating-webhook)'
  curl -s http://<任意NPU节点>:9400/metrics | grep ascend_container_hbm_live_bytes

工作负载接入：直接按原样下发 YAML（webhook 会自动注入）。
历史版本的手动注入方式（deploy/inject.py）仍适用于存量 Deployment。

卸载: bash scripts/uninstall.sh
TIP
