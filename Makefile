# ascend-container-exporter Makefile
# 约定：shim 必须在与目标节点 glibc 相同或更旧的环境编译（推荐直接在 NPU 节点上编译）

VERSION ?= 0.2.0
IMAGE ?= ascend-container-exporter:v$(VERSION)
REGISTRY ?= your-registry.example.com   # 改成你的镜像仓库

.PHONY: shim exporter image deploy inject clean

## 在 NPU 节点（或任意 aarch64 Linux + gcc，glibc 不新于目标节点）上执行
shim:
	gcc -shared -fPIC -O2 -Wall -o libnpu-account.so shim/npu_shim.c -ldl -lpthread -lrt
	@echo "==> libnpu-account.so (把产物放到本项目根目录再 make image)"

## 在任意有 Go 1.25+ 的开发机执行（交叉编译 linux/arm64 静态二进制）
exporter:
	cd exporter && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o ../ascend-container-exporter .
	@echo "==> ascend-container-exporter (linux/arm64 静态)"

## 需要先 make shim exporter 产出两个文件；在能访问 busybox 基础镜像的机器上执行
image:
	docker build -f deploy/Dockerfile -t $(REGISTRY)/$(IMAGE) .
	@echo "==> $(REGISTRY)/$(IMAGE)  (推送: docker push $(REGISTRY)/$(IMAGE))"

## 部署 DaemonSet（先把 deploy/ascend-container-exporter.yaml 里的镜像改成本步产物）
deploy:
	kubectl apply -f deploy/ascend-container-exporter.yaml

## 给存量 Deployment 注入 LD_PRELOAD + 挂载（幂等，可重复执行）
inject:
	python3 deploy/inject.py $(DEPLOY)

clean:
	rm -f libnpu-account.so ascend-container-exporter
