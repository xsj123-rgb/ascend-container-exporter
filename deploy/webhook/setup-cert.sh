#!/usr/bin/env bash
# 生成 webhook 自签证书 → 建 Secret → 把 caBundle 写进 MutatingWebhookConfiguration
# 用法: WEBHOOK_IMAGE=<镜像> bash setup-cert.sh
set -euo pipefail
NS=monitoring
SVC=ascend-mutating-webhook
IMG=${WEBHOOK_IMAGE:?请设置 WEBHOOK_IMAGE，如 your-registry.example.com/ascend-mutating-webhook:v0.3.0}
WORK=$(mktemp -d)

# 1) CA + 服务端证书（SAN 必须含 Service 的集群内 DNS）
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

# 2) Secret
kubectl -n "$NS" create secret tls ascend-webhook-certs \
  --cert="$WORK/tls.crt" --key="$WORK/tls.key" --dry-run=client -o yaml | kubectl apply -f -

# 3) MutatingWebhookConfiguration（caBundle 内联替换）
CABUNDLE=$(base64 < "$WORK/ca.crt" | tr -d '\n')
sed "s|__CABUNDLE__|$CABUNDLE|; s|__IMAGE__|$IMG|" \
  "$(dirname "$0")/webhook.yaml" | kubectl apply -f -

echo "==> done: webhook deployed, check: kubectl -n $NS get pods -l app=ascend-mutating-webhook"
