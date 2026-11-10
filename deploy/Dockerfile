# 推荐：与「离线/内网」构建方式一致（先 make shim && make exporter，产物放本目录）
# busybox 基础镜像需在本机 docker images 中存在，或改为你内网仓库中的等价镜像
FROM docker.io/library/busybox:latest
COPY ascend-container-exporter /usr/local/bin/ascend-container-exporter
COPY libnpu-account.so /opt/npu-account/libnpu-account.so
ENTRYPOINT ["/usr/local/bin/ascend-container-exporter"]
