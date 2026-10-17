#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static FILE *g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int g_patch = 1; /* set NVIOC_PATCH=0 to log-only */

static void init(void) {
    if (g_log) return;
    const char *path = getenv("NVIOC_LOG");
    if (!path) path = "/tmp/nvpowerd_ioctl.log";
    g_log = fopen(path, "a");
    if (!g_log) g_log = stderr;
    setvbuf(g_log, NULL, _IOLBF, 0);
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    const char *p = getenv("NVIOC_PATCH");
    if (p && atoi(p) == 0) g_patch = 0;
    fprintf(g_log, "[interposer v3] started pid=%d log=%s patch=%d\n", getpid(), path, g_patch);
}

static void ts(char *buf, size_t n) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm; localtime_r(&t.tv_sec, &tm);
    snprintf(buf, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec/1000000);
}

static const char *rm_name(uint32_t cmd) {
    switch (cmd) {
    case 0x2080a618: return "NVPCF_GET_ABS_POWER_LIMITS";
    case 0x2080a61a: return "NVPCF_GET_POWER_POLICY";
    case 0x2080a61f: return "NVPCF_GET_POLICY_STATUS";
    case 0x2080a612: return "GET_POWER_MONITOR";
    case 0x2080e634: return "SET_GET_POWER_LIMIT(E634)";
    default: return "";
    }
}

static int is_power_like(uint32_t v) {
    return v >= 5000 && v <= 400000 && (v % 1000) == 0;
}

/* parse NVOS54: +0 hClient, +4 hSubDev, +8 cmd, +12 flags, +16 params*, +24 paramsSize */
static int parse_nvos54(const void *arg, uint32_t *cmd, void **params, uint32_t *psize) {
    const uint8_t *p = arg;
    memcpy(cmd, p + 8, 4);
    uint64_t ptr; uint32_t sz;
    memcpy(&ptr, p + 16, 8);
    memcpy(&sz, p + 24, 4);
    *params = (void *)ptr;
    *psize = sz;
    return (*cmd & 0xffff0000) == 0x20800000;
}

/* scan a buffer for power-like dwords and log them with offsets */
static void log_powerish(const char *tag, const void *buf, size_t n) {
    const uint32_t *dw = buf;
    size_t cnt = n / 4;
    int any = 0;
    for (size_t i = 0; i < cnt; i++) {
        if (is_power_like(dw[i])) {
            if (!any) { fprintf(g_log, "    %s POWERISH:", tag); any = 1; }
            fprintf(g_log, " [%zu]=%u", i, dw[i]);
        }
    }
    if (any) fprintf(g_log, "\n");
}

/*
 * Patch 1: NVPCF_GET_ABS_POWER_LIMITS POST — rewrite the dynamic-boost delta
 * table at the tail (dwords 5077/5079/5081/5083 = 55k/45k/35k/25k) by adding
 * BOOST_ADD mW to each so the active boost becomes 25W+25W=50W.
 */
static void patch_abs_limits_post(void *params, uint32_t psize) {
    uint32_t *dw = params;
    size_t cnt = psize / 4;
    /* the four boost tiers sit just before a 0x7fffffff sentinel at the tail;
       locate the known pattern rather than hardcoding only offsets */
    const uint32_t want[4] = {55000, 45000, 35000, 25000};
    const uint32_t add = 25000;
    for (size_t i = 0; i + 3 < cnt; i++) {
        if (dw[i] == want[0] && dw[i+1] == want[1] && dw[i+2] == want[2] && dw[i+3] == want[3]) {
            fprintf(g_log, "  [PATCH] ABS_LIMITS boost tiers at dwords %zu..%zu: %u,%u,%u,%u", i, i+3, dw[i], dw[i+1], dw[i+2], dw[i+3]);
            if (g_patch) {
                dw[i]   += add;
                dw[i+1] += add;
                dw[i+2] += add;
                dw[i+3] += add;
            }
            fprintf(g_log, " -> %u,%u,%u,%u%s\n", dw[i], dw[i+1], dw[i+2], dw[i+3], g_patch ? "" : " (no-patch)");
            return;
        }
    }
    fprintf(g_log, "  [PATCH] ABS_LIMITS boost tier pattern NOT found\n");
}

/*
 * Patch 2: outgoing E634 set-limit — if dword[11] carries 175000, bump to 200000.
 * More generally: if it equals base(150000)+old_boost(25000), write base+new(200000).
 */
static void patch_e634_pre(void *params, uint32_t psize) {
    uint32_t *dw = params;
    size_t cnt = psize / 4;
    if (cnt < 12) return;
    uint32_t v = dw[11];
    if (v == 175000) {
        fprintf(g_log, "  [PATCH] E634 set-limit dword[11]=175000");
        if (g_patch) dw[11] = 200000;
        fprintf(g_log, " -> %u%s\n", dw[11], g_patch ? "" : " (no-patch)");
    } else if (is_power_like(v)) {
        fprintf(g_log, "  E634 set-limit dword[11]=%u (left as-is)\n", v);
    }
}

int open(const char *path, int flags, ...) {
    init();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_open(path, flags, mode);
    if (path && (strstr(path, "nvidia") || strstr(path, "nvli") || strstr(path, "nvsw"))) {
        char t[32]; ts(t, sizeof t);
        pthread_mutex_lock(&g_lock);
        fprintf(g_log, "%s open(\"%s\") = %d\n", t, path, fd);
        pthread_mutex_unlock(&g_lock);
    }
    return fd;
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    init();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_openat(dirfd, path, flags, mode);
    if (path && (strstr(path, "nvidia") || strstr(path, "nvli") || strstr(path, "nvsw"))) {
        char t[32]; ts(t, sizeof t);
        pthread_mutex_lock(&g_lock);
        fprintf(g_log, "%s openat(\"%s\") = %d\n", t, path, fd);
        pthread_mutex_unlock(&g_lock);
    }
    return fd;
}

int ioctl(int fd, unsigned long request, ...) {
    init();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    char link[64], tgt[128];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(link, tgt, sizeof tgt - 1);
    int is_nv = 0;
    if (l > 0) {
        tgt[l] = 0;
        if (strstr(tgt, "nvidia") || strstr(tgt, "nvli") || strstr(tgt, "nvsw")) is_nv = 1;
    }
    if (!is_nv) return real_ioctl(fd, request, arg);

    uint32_t cmd = 0; void *params = NULL; uint32_t psize = 0;
    int have = (request == 0xc020462a) && arg && parse_nvos54(arg, &cmd, &params, &psize);

    char t[32]; ts(t, sizeof t);
    pthread_mutex_lock(&g_lock);
    if (have) {
        int interesting = (cmd == 0x2080a618 || cmd == 0x2080e634 || cmd == 0x2080a61a || cmd == 0x2080a61f);
        if (interesting) {
            fprintf(g_log, "%s RM cmd=0x%08x %s params=%p size=0x%x\n", t, cmd, rm_name(cmd), params, psize);
        }
        /* patch outgoing set-limit BEFORE the driver sees it */
        if (have && params && psize >= 48 && cmd == 0x2080e634) {
            patch_e634_pre(params, psize);
        }
    }
    fflush(g_log);
    pthread_mutex_unlock(&g_lock);

    int ret = real_ioctl(fd, request, arg);

    pthread_mutex_lock(&g_lock);
    if (have && params && psize > 0 && psize <= 0x20000) {
        if (cmd == 0x2080a618) {
            /* patch boost deltas in the POST buffer before powerd reads it */
            patch_abs_limits_post(params, psize);
            log_powerish("ABS_POST", params, psize);
        } else if (cmd == 0x2080e634) {
            log_powerish("E634_POST", params, psize);
        } else if (cmd == 0x2080a61a || cmd == 0x2080a61f) {
            log_powerish("NVPCF_POST", params, psize);
        }
    }
    fflush(g_log);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

__attribute__((constructor)) static void ctor(void) { init(); }
