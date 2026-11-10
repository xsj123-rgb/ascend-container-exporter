# 离线/内网构建（与 README 快速开始一致）：先 make shim && make build，产物放 bin/ 与根目录
# busybox 需在本机 docker images 中存在，或改为你内网仓库中的等价镜像
FROM docker.io/library/busybox:latest
COPY bin/ascend-container-exporter /usr/local/bin/ascend-container-exporter
COPY bin/ascend-mutating-webhook /usr/local/bin/ascend-mutating-webhook
COPY libnpu-account.so /opt/npu-account/libnpu-account.so
# 默认入口 = 探针 DaemonSet；webhook Deployment 以 command 覆盖启动
ENTRYPOINT ["/usr/local/bin/ascend-container-exporter"]
