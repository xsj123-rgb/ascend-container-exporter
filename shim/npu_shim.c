/*
 * libnpu-account v0.5.0 — LD_PRELOAD 显存记账 shim
 *
 * 双层拦截，覆盖 CANN 新旧两代分配路径：
 *   ① libruntime 的 rt* 内部 ABI（rtMalloc/rtMemAlloc/rtMallocPhysical 等）—— CANN ≤ 9.1 的路径；
 *   ② libascendcl 的 ACL vMM 公共 API（aclrtMalloc/aclrtMallocPhysical 等）——
 *      CANN 9.2.0 起版本化运行时(rt2 registry)不再调用 rt*，torch_npu 经 PLT 直连 ACL 层。
 *
 * 防双计：老栈上「torch→aclrtMalloc(外层)→rtMalloc(内层)」同笔分配会同时命中两层——
 * ACL 钩子记账后写线程局部缓存 (key,size)，内层 rt 钩子发现精确匹配则只透传不记账；
 * 释放方向天然安全（F 按键查表删除，外层先删则内层自动 no-op）。
 *
 * 事件流 JSONL 落盘：{dir}/{container_id}.jsonl（dir 默认 /run/npu-account，NPU_ACCOUNT_DIR 覆盖）
 *   {"t":"I","pid":..,"cid":".."}
 *   {"t":"M","pid":..,"sym":"aclrtMallocPhysical|rtMalloc|...","key":"0x..","size":..}
 *   {"t":"F","pid":..,"sym":"...","key":"0x..","size":..}
 *   {"t":"S","pid":..,"hpid":..,"live":..,"total":..,"freed":..}   每事件后快照
 *   {"t":"X","pid":..,"hpid":..,"live":..,...}
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

/* ---- libruntime 内部 ABI（CANN ≤ 9.1 路径） ---- */
static uint64_t (*real_rtMalloc)(void **, uint64_t, uint32_t, uint16_t);
static uint64_t (*real_rtMallocCached)(void **, uint64_t, uint32_t, uint16_t);
static uint64_t (*real_rtMemAlloc)(void **, uint64_t, uint32_t, uint32_t, uint64_t);
static uint64_t (*real_rtMallocPhysical)(uint64_t *, uint64_t, void *, uint64_t);
static uint64_t (*real_rtFree)(void *);
static uint64_t (*real_rtsFree)(void *);
static uint64_t (*real_rtFreePhysical)(uint64_t);

/* ---- libascendcl ACL vMM API（CANN 9.2+ 路径，aclError=int） ---- */
static int (*real_aclrtMalloc)(void **, uint64_t, int);
static int (*real_aclrtMallocPhysical)(uint64_t *, uint64_t, void *, uint64_t); /* 4 参: handle,size,prop,flags */
static int (*real_aclrtFree)(void *);
static int (*real_aclrtFreePhysical)(uint64_t);

/* ---- 防双计：ACL 层刚记过账的 (key,size)，内层 rt 钩子精确匹配则跳过 ---- */
static __thread void  *t_dedup_key;
static __thread uint64_t t_dedup_sz;

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

static void *resolve_acl(const char *sym)
{
    void *p = dlsym(RTLD_NEXT, sym);
    if (p) return p;
    static void *h;
    if (!h) {
        h = dlopen("libascendcl.so", RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen("libascendcl.so", RTLD_LAZY);
    }
    return h ? dlsym(h, sym) : NULL;
}

/* rt 层记账前查防双计缓存：与 ACL 层刚记的 (key,size) 精确匹配则跳过 */
static int dedup_hit(void *key, uint64_t sz)
{
    if (t_dedup_key == key && t_dedup_sz == sz && t_dedup_key != NULL) {
        t_dedup_key = NULL;
        t_dedup_sz = 0;
        return 1;
    }
    return 0;
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

/* ---- 拦截层①：libruntime rt*（CANN ≤ 9.1 路径；ACL 层已记过的同笔分配自动跳过） ---- */

uint64_t rtMalloc(void **devPtr, uint64_t size, uint32_t t, uint16_t moduleId)
{
    if (!real_rtMalloc) real_rtMalloc = (void *)resolve_rt("rtMalloc");
    if (!real_rtMalloc) return 0xFFFFFFFF;
    uint64_t r = real_rtMalloc(devPtr, size, t, moduleId);
    if (r == 0 && devPtr && *devPtr && !dedup_hit(*devPtr, size))
        do_record_alloc("rtMalloc", *devPtr, size);
    return r;
}

uint64_t rtMallocCached(void **devPtr, uint64_t size, uint32_t t, uint16_t moduleId)
{
    if (!real_rtMallocCached) real_rtMallocCached = (void *)resolve_rt("rtMallocCached");
    if (!real_rtMallocCached) return 0xFFFFFFFF;
    uint64_t r = real_rtMallocCached(devPtr, size, t, moduleId);
    if (r == 0 && devPtr && *devPtr && !dedup_hit(*devPtr, size))
        do_record_alloc("rtMallocCached", *devPtr, size);
    return r;
}

uint64_t rtMemAlloc(void **devPtr, uint64_t size, uint32_t policy, uint32_t flags, uint64_t cfg)
{
    if (!real_rtMemAlloc) real_rtMemAlloc = (void *)resolve_rt("rtMemAlloc");
    if (!real_rtMemAlloc) return 0xFFFFFFFF;
    uint64_t r = real_rtMemAlloc(devPtr, size, policy, flags, cfg);
    if (r == 0 && devPtr && *devPtr && !dedup_hit(*devPtr, size))
        do_record_alloc("rtMemAlloc", *devPtr, size);
    return r;
}

uint64_t rtMallocPhysical(uint64_t *handle, uint64_t size, void *prop, uint64_t flags)
{
    if (!real_rtMallocPhysical) real_rtMallocPhysical = (void *)resolve_rt("rtMallocPhysical");
    if (!real_rtMallocPhysical) return 0xFFFFFFFF;
    uint64_t r = real_rtMallocPhysical(handle, size, prop, flags);
    if (r == 0 && handle && *handle && !dedup_hit((void *)(uintptr_t)*handle, size))
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

/* ---- 拦截层②：libascendcl ACL vMM API（CANN 9.2+ 路径） ---- */

int aclrtMalloc(void **devPtr, uint64_t size, int policy)
{
    if (!real_aclrtMalloc) real_aclrtMalloc = (void *)resolve_acl("aclrtMalloc");
    if (!real_aclrtMalloc) return 107000;
    int r = real_aclrtMalloc(devPtr, size, policy);
    if (r == 0 && devPtr && *devPtr) {
        t_dedup_key = *devPtr; t_dedup_sz = size;   /* 供内层 rt 钩子去重 */
        do_record_alloc("aclrtMalloc", *devPtr, size);
    }
    return r;
}

int aclrtMallocPhysical(uint64_t *handle, uint64_t size, void *prop, uint64_t flags)
{
    if (!real_aclrtMallocPhysical) real_aclrtMallocPhysical = (void *)resolve_acl("aclrtMallocPhysical");
    if (!real_aclrtMallocPhysical) return 107000;
    int r = real_aclrtMallocPhysical(handle, size, prop, flags);
    if (r == 0 && handle && *handle) {
        t_dedup_key = (void *)(uintptr_t)*handle; t_dedup_sz = size;
        do_record_alloc("aclrtMallocPhysical", (void *)(uintptr_t)*handle, size);
    }
    return r;
}

int aclrtFree(void *ptr)
{
    if (!real_aclrtFree) real_aclrtFree = (void *)resolve_acl("aclrtFree");
    if (!real_aclrtFree) return 107000;
    int r = real_aclrtFree(ptr);
    if (r == 0 && ptr) do_record_free("aclrtFree", ptr);
    return r;
}

int aclrtFreePhysical(uint64_t handle)
{
    if (!real_aclrtFreePhysical) real_aclrtFreePhysical = (void *)resolve_acl("aclrtFreePhysical");
    if (!real_aclrtFreePhysical) return 107000;
    int r = real_aclrtFreePhysical(handle);
    if (r == 0 && handle) do_record_free("aclrtFreePhysical", (void *)(uintptr_t)handle);
    return r;
}
