/*
 * libnpu-account v0.2.0 — LD_PRELOAD 显存记账 shim
 *
 * 拦截 libruntime.so 的设备显存分配 ABI（rtMalloc/rtMemAlloc/rtMallocPhysical 等，
 * 符号集与 hami-vnpu-core 生产实践一致），按事件流 JSONL 落盘：
 *   {dir}/{container_id}.jsonl
 *   dir 默认 /run/npu-account（Pod 内 hostPath 挂载同路径），可用 NPU_ACCOUNT_DIR 覆盖
 *
 * 事件行（单次 write + O_APPEND，进程间不交错）：
 *   {"t":"I","pid":..,"cid":".."}                              进程加载 shim
 *   {"t":"M","pid":..,"sym":"rtMalloc","key":"0x..","size":..} 分配
 *   {"t":"F","pid":..,"sym":"rtFree","key":"0x..","size":..}   释放
 *   {"t":"S","pid":..,"hpid":..,"live":..,"total":..,"freed":..} 每事件后快照(聚合器取每 pid 最后一条)
 *   {"t":"X","pid":..,"hpid":..,"live":..,...}                  进程退出
 *
 * 编译（在目标节点或 glibc 不新于目标节点的 aarch64 环境执行）:
 *   gcc -shared -fPIC -O2 -Wall -o libnpu-account.so npu_shim.c -ldl -lpthread -lrt
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- libruntime 内部 ABI ---- */
static uint64_t (*real_rtMalloc)(void **, uint64_t, uint32_t, uint16_t);
static uint64_t (*real_rtMallocCached)(void **, uint64_t, uint32_t, uint16_t);
static uint64_t (*real_rtMemAlloc)(void **, uint64_t, uint32_t, uint32_t, uint64_t);
static uint64_t (*real_rtMallocPhysical)(uint64_t *, uint64_t, void *, uint64_t);
static uint64_t (*real_rtFree)(void *);
static uint64_t (*real_rtsFree)(void *);
static uint64_t (*real_rtFreePhysical)(uint64_t);

/* ---- 记账状态（进程内） ---- */
#define MAX_KEYS 65536
static void *g_keys[MAX_KEYS];
static uint64_t g_sz[MAX_KEYS];
static uint64_t g_live = 0, g_total = 0, g_freed = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;          /* JSONL 事件流 */
static int g_hpid = 0;         /* 宿主机全局 pid */
static char g_cid[80] = "unknown";

static void jwrite(const char *line)
{
    if (g_fd < 0) return;
    ssize_t rc = write(g_fd, line, strlen(line));
    (void)rc;
}

static void emit_alloc(const char *sym, void *key, uint64_t sz)
{
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
        "{\"t\":\"M\",\"pid\":%d,\"sym\":\"%s\",\"key\":\"%p\",\"size\":%llu}\n"
        "{\"t\":\"S\",\"pid\":%d,\"hpid\":%d,\"live\":%llu,\"total\":%llu,\"freed\":%llu}\n",
        getpid(), sym, key, (unsigned long long)sz,
        getpid(), g_hpid, (unsigned long long)(g_live),
        (unsigned long long)g_total, (unsigned long long)g_freed);
    if (n > 0) jwrite(buf);
}

static void emit_free(const char *sym, void *key, uint64_t sz)
{
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
        "{\"t\":\"F\",\"pid\":%d,\"sym\":\"%s\",\"key\":\"%p\",\"size\":%llu}\n"
        "{\"t\":\"S\",\"pid\":%d,\"hpid\":%d,\"live\":%llu,\"total\":%llu,\"freed\":%llu}\n",
        getpid(), sym, key, (unsigned long long)sz,
        getpid(), g_hpid, (unsigned long long)(g_live),
        (unsigned long long)g_total, (unsigned long long)g_freed);
    if (n > 0) jwrite(buf);
}

/* 从 /proc/self/cgroup 自发现容器 ID（containerd: cri-containerd-<64hex>.scope） */
static void discover_cid(void)
{
    const char *ovr = getenv("NPU_ACCOUNT_ID");
    if (ovr && ovr[0]) {
        snprintf(g_cid, sizeof(g_cid), "%s", ovr);
        return;
    }
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        for (const char *tag = "cri-containerd-"; ; ) {
            const char *p = strstr(line, tag);
            if (!p) break;
            p += strlen(tag);
            size_t i = 0;
            while (i < 64 && ((p[i] >= '0' && p[i] <= '9') ||
                              (p[i] >= 'a' && p[i] <= 'f'))) i++;
            if (i == 64) {
                snprintf(g_cid, sizeof(g_cid), "%.*s", 64, p);
                fclose(f);
                return;
            }
            tag = "\x01"; /* 本行不再匹配 */
            break;
        }
    }
    fclose(f);
}

/* 容器内读宿主机全局 PID：/proc/self/sched 首字段在 pid namespace 内仍是全局值 */
static int host_pid(void)
{
    FILE *f = fopen("/proc/self/sched", "r");
    if (!f) return 0;
    char name[64];
    int hp = 0;
    if (fscanf(f, "%63s (%d)", name, &hp) < 2) hp = 0;
    fclose(f);
    return hp;
}

static void *resolve_rt(const char *sym)
{
    void *p = dlsym(RTLD_NEXT, sym);
    if (p) return p;
    static void *h;
    if (!h) {
        h = dlopen("libruntime.so", RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen("libruntime.so", RTLD_LAZY);
    }
    return h ? dlsym(h, sym) : NULL;
}

__attribute__((constructor)) static void ctor(void)
{
    discover_cid();
    g_hpid = host_pid();
    const char *dir = getenv("NPU_ACCOUNT_DIR");
    if (!dir || !dir[0]) dir = "/run/npu-account";
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.jsonl", dir, g_cid);
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    /* 目录不存在时降级 /tmp，保证业务不受影响 */
    if (g_fd < 0) {
        snprintf(path, sizeof(path), "/tmp/npu-account-%s.jsonl", g_cid);
        g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    if (g_fd >= 0) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf),
            "{\"t\":\"I\",\"pid\":%d,\"cid\":\"%.64s\"}\n", getpid(), g_cid);
        if (n > 0) jwrite(buf);
    }
}

__attribute__((destructor)) static void dtor(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_fd >= 0) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf),
            "{\"t\":\"X\",\"pid\":%d,\"hpid\":%d,\"live\":%llu,\"total\":%llu,\"freed\":%llu}\n",
            getpid(), g_hpid, (unsigned long long)g_live,
            (unsigned long long)g_total, (unsigned long long)g_freed);
        if (n > 0) jwrite(buf);
        close(g_fd);
        g_fd = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void do_record_alloc(const char *sym, void *key, uint64_t sz)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_KEYS; i++) {
        if (g_keys[i] == NULL) {
            g_keys[i] = key;
            g_sz[i] = sz;
            g_live += sz;
            g_total += sz;
            emit_alloc(sym, key, sz);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void do_record_free(const char *sym, void *key)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_KEYS; i++) {
        if (g_keys[i] == key) {
            g_live -= g_sz[i];
            g_freed += g_sz[i];
            emit_free(sym, key, g_sz[i]);
            g_keys[i] = NULL;
            g_sz[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---- 拦截层 ---- */

uint64_t rtMalloc(void **devPtr, uint64_t size, uint32_t t, uint16_t moduleId)
{
    if (!real_rtMalloc) real_rtMalloc = (void *)resolve_rt("rtMalloc");
    if (!real_rtMalloc) return 0xFFFFFFFF;
    uint64_t r = real_rtMalloc(devPtr, size, t, moduleId);
    if (r == 0 && devPtr && *devPtr) do_record_alloc("rtMalloc", *devPtr, size);
    return r;
}

uint64_t rtMallocCached(void **devPtr, uint64_t size, uint32_t t, uint16_t moduleId)
{
    if (!real_rtMallocCached) real_rtMallocCached = (void *)resolve_rt("rtMallocCached");
    if (!real_rtMallocCached) return 0xFFFFFFFF;
    uint64_t r = real_rtMallocCached(devPtr, size, t, moduleId);
    if (r == 0 && devPtr && *devPtr) do_record_alloc("rtMallocCached", *devPtr, size);
    return r;
}

uint64_t rtMemAlloc(void **devPtr, uint64_t size, uint32_t policy, uint32_t flags, uint64_t cfg)
{
    if (!real_rtMemAlloc) real_rtMemAlloc = (void *)resolve_rt("rtMemAlloc");
    if (!real_rtMemAlloc) return 0xFFFFFFFF;
    uint64_t r = real_rtMemAlloc(devPtr, size, policy, flags, cfg);
    if (r == 0 && devPtr && *devPtr) do_record_alloc("rtMemAlloc", *devPtr, size);
    return r;
}

uint64_t rtMallocPhysical(uint64_t *handle, uint64_t size, void *prop, uint64_t flags)
{
    if (!real_rtMallocPhysical) real_rtMallocPhysical = (void *)resolve_rt("rtMallocPhysical");
    if (!real_rtMallocPhysical) return 0xFFFFFFFF;
    uint64_t r = real_rtMallocPhysical(handle, size, prop, flags);
    if (r == 0 && handle && *handle)
        do_record_alloc("rtMallocPhysical", (void *)(uintptr_t)*handle, size);
    return r;
}

uint64_t rtFree(void *ptr)
{
    if (!real_rtFree) real_rtFree = (void *)resolve_rt("rtFree");
    if (!real_rtFree) return 0xFFFFFFFF;
    uint64_t r = real_rtFree(ptr);
    if (r == 0 && ptr) do_record_free("rtFree", ptr);
    return r;
}

uint64_t rtsFree(void *ptr)
{
    if (!real_rtsFree) real_rtsFree = (void *)resolve_rt("rtsFree");
    if (!real_rtsFree) return 0xFFFFFFFF;
    uint64_t r = real_rtsFree(ptr);
    if (r == 0 && ptr) do_record_free("rtsFree", ptr);
    return r;
}

uint64_t rtFreePhysical(uint64_t handle)
{
    if (!real_rtFreePhysical) real_rtFreePhysical = (void *)resolve_rt("rtFreePhysical");
    if (!real_rtFreePhysical) return 0xFFFFFFFF;
    uint64_t r = real_rtFreePhysical(handle);
    if (r == 0 && handle) do_record_free("rtFreePhysical", (void *)(uintptr_t)handle);
    return r;
}
