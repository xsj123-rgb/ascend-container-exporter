#!/bin/bash
# 排障工具：分析 LD_DEBUG=bindings 日志，确认拦截符号的绑定去向
# 用法（在业务容器内）:
#   env LD_PRELOAD=/opt/npu-account/libnpu-account.so LD_DEBUG=bindings <app> 2>/tmp/bindings.log
#   然后把日志拷到本机执行本脚本，看各分配符号到底绑给了谁
LOG=${1:-/tmp/bindings.log}
echo "=== 1. 精确符号 aclrtMalloc 的绑定(含来源与目标) ==="
grep -F 'symbol `aclrtMalloc'"'" /tmp/bindings.log 2>/dev/null | head -8 || \
grep "symbol .aclrtMalloc.$" $LOG | head -8

echo ""
echo "=== 2. 绑定到 npu_shim 的全部精确符号统计 ==="
grep "to /tmp/npu_shim.so" $LOG | awk -F'symbol ' '{print $2}' | sort | uniq -c | sort -rn | head -20

echo ""
echo "=== 3. 各 malloc 家族符号的绑定去向 ==="
for sym in aclrtMallocHost aclrtMallocCached aclrtMallocPhysical aclrtMallocWithCfg aclrtMallocAlign32 rtMalloc rtMemAlloc; do
  echo "-- $sym:"
  grep "symbol .${sym}.$" $LOG | grep -oE "to [^ ]+ \[" | sort | uniq -c | sort -rn | head -3
done
