#!/usr/bin/env python3
"""注入 LD_PRELOAD 记账到指定 Deployment（幂等，可重复执行）
用法:
    NS=<namespace> python3 inject.py <deployment-1> [deployment-2 ...]
- 已有 LD_PRELOAD（如 jemalloc）→ 冒号追加路径而非覆盖
- Deployment 无 volumeMounts/volumes 键 → 自动创建数组
"""
import json, os, subprocess, sys, time

NS = os.environ.get("NS", "default")
SHIM = "/opt/npu-account/libnpu-account.so"

def kubectl_json(*args):
    r = subprocess.run(["kubectl", "-n", NS] + list(args), capture_output=True, text=True)
    return json.loads(r.stdout) if r.returncode == 0 else None

def patch(name, ops):
    r = subprocess.run(["kubectl", "-n", NS, "patch", "deploy", name,
                        "--type=json", "-p", json.dumps(ops)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{name}: PATCH FAILED: {r.stderr.strip()[:150]}")
        return False
    return True

def inject(name):
    d = kubectl_json("get", "deploy", name, "-o", "json")
    if not d:
        print(f"{name}: NOT FOUND"); return False
    s = d["spec"]["template"]["spec"]
    c = s["containers"][0]
    envs = c.get("env", [])
    mounts = c.get("volumeMounts", [])
    vols = s.get("volumes", [])
    ops = []

    cur = next((e["value"] for e in envs if e["name"] == "LD_PRELOAD"), None)
    if cur is None:
        if envs:
            ops.append({"op": "add", "path": "/spec/template/spec/containers/0/env/-",
                        "value": {"name": "LD_PRELOAD", "value": SHIM}})
        else:  # env 键都不存在
            ops.append({"op": "add", "path": "/spec/template/spec/containers/0/env",
                        "value": [{"name": "LD_PRELOAD", "value": SHIM}]})
    elif SHIM in cur:
        print(f"{name}: already injected, skip"); return True
    else:  # 已有 jemalloc 等 → 追加
        idx = next(i for i, e in enumerate(envs) if e["name"] == "LD_PRELOAD")
        ops.append({"op": "replace",
                    "path": f"/spec/template/spec/containers/0/env/{idx}",
                    "value": {"name": "LD_PRELOAD", "value": cur + ":" + SHIM}})

    mpaths = [m["mountPath"] for m in mounts]
    want_mounts = [
        {"name": "npu-account-lib", "mountPath": "/opt/npu-account", "readOnly": True},
        {"name": "npu-account-data", "mountPath": "/run/npu-account"},  # 必须可写
    ]
    for wm in want_mounts:
        if wm["mountPath"] in mpaths:
            continue
        if mounts:
            ops.append({"op": "add", "path": "/spec/template/spec/containers/0/volumeMounts/-",
                        "value": wm})
        else:
            ops.append({"op": "add", "path": "/spec/template/spec/containers/0/volumeMounts",
                        "value": [wm]})
            mounts = [wm]  # 后续走 add /-

    vnames = [v["name"] for v in vols]
    want_vols = [
        {"name": "npu-account-lib", "hostPath": {"path": "/opt/npu-account", "type": "Directory"}},
        {"name": "npu-account-data", "hostPath": {"path": "/run/npu-account", "type": "Directory"}},
    ]
    for wv in want_vols:
        if wv["name"] in vnames:
            continue
        if vols:
            ops.append({"op": "add", "path": "/spec/template/spec/volumes/-", "value": wv})
        else:
            ops.append({"op": "add", "path": "/spec/template/spec/volumes", "value": [wv]})
            vols = [wv]

    if not ops:
        print(f"{name}: nothing to do"); return True
    ok = patch(name, ops)
    if ok:
        print(f"{name}: patched ({len(ops)} ops) LD_PRELOAD="
              f"{(cur + ':' + SHIM) if cur else SHIM}")
    return ok

def wait_rollout(name, timeout=420):
    """等待滚动完成 + 新 pod 无重启"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = kubectl_json("get", "deploy", name, "-o", "json")
        if d:
            r, u = d["status"].get("readyReplicas", 0), d["status"].get("updatedReplicas", 0)
            total = d["spec"].get("replicas", 1)
            if r == total and u == total:
                r2 = subprocess.run(["kubectl", "-n", NS, "get", "pods", "-l",
                                     d["spec"]["selector"]["matchLabels"].get("app", name),
                                     "-o", "jsonpath={.items[0].status.containerStatuses[0].restartCount}"],
                                    capture_output=True, text=True)
                if r2.stdout.strip() in ("0", ""):
                    print(f"{name}: rollout OK ({int(time.time()-t0)}s)")
                    return True
        time.sleep(15)
    print(f"{name}: rollout TIMEOUT")
    return False

if __name__ == "__main__":
    for name in sys.argv[1:]:
        if inject(name):
            wait_rollout(name)
        else:
            sys.exit(1)
