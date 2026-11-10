# ascend-container-exporter Makefile
# 常用: make build → make image → make install   （卸载: make uninstall）

VERSION ?= 0.4.0
REGISTRY ?= your-registry.example.com
IMAGE ?= $(REGISTRY)/ascend-container-exporter:v$(VERSION)

.PHONY: shim build image install uninstall inject clean

## 在 NPU 节点（或任意 aarch64 Linux + gcc，glibc 不新于目标节点）上执行
shim:
	gcc -shared -fPIC -O2 -Wall -o libnpu-account.so shim/npu_shim.c -ldl -lpthread -lrt
	@echo "==> libnpu-account.so"

## 开发机执行：编译两个二进制（linux/arm64 静态）
build: exporter webhook
	@echo "==> bin/ascend-container-exporter + bin/ascend-mutating-webhook"

exporter:
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o bin/ascend-container-exporter ./cmd/exporter

webhook:
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o bin/ascend-mutating-webhook ./cmd/webhook

## 需要先 make shim build（产物：libnpu-account.so + bin/*）
image:
	docker build -f deploy/Dockerfile -t $(IMAGE) .
	@echo "==> $(IMAGE)  (推送: docker push $(IMAGE))"

## 一键安装：DS + webhook + 证书（用户唯一需要执行的安装命令）
install:
	IMAGE=$(IMAGE) bash scripts/install.sh

uninstall:
	bash scripts/uninstall.sh

## 存量 Deployment 批量注入（装了 webhook 后通常不再需要）
inject:
	NS=$(NS) python3 deploy/inject.py $(DEPLOY)

clean:
	rm -rf bin libnpu-account.so
