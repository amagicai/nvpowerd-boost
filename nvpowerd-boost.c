/*
 * nvpowerd-boost.c — LD_PRELOAD interposer that raises nvidia-powerd's
 * Dynamic Boost ceiling on Linux laptops.
 *
 * BACKGROUND
 *   nvidia-powerd 2.0 (driver 535+) has no user config file. It asks the
 *   kernel driver (via NVPCF RM ioctls) for the platform power table, which
 *   the driver sources from the VBIOS / SBIOS ACPI. The daemon then applies
 *   (base TGP + dynamic-boost delta) as the GPU power limit.
 *
 *   This library intercepts two RM control calls:
 *
 *     1. 0x2080a618 NVPCF_GET_ABS_POWER_LIMITS (POST, driver -> daemon)
 *        The response contains the base/max limits and a quartet of boost
 *        deltas (stock: 55000/45000/35000/25000 mW). NVIOC_BOOST_DELTA mW is
 *        added to each tier, so the daemon computes a higher setpoint.
 *
 *     2. 0x2080e634 set-power-limit (PRE, daemon -> driver)
 *        If the outgoing limit is base+stock_delta it is raised to
 *        base+stock_delta+NVIOC_BOOST_DELTA, and any outgoing value above the
 *        driver-reported maximum is clamped to that maximum.
 *
 *   One log line is emitted each time the effective GPU limit changes.
 *
 * USAGE
 *   sudo NVIOC_LOG=/tmp/nvpowerd.log NVIOC_BOOST_DELTA=25000 \
 *        LD_PRELOAD=./nvpowerd-boost.so /usr/bin/nvidia-powerd
 *
 * ENVIRONMENT
 *   NVIOC_BOOST_DELTA  mW added to each boost tier   (default 0 = stock)
 *   NVIOC_LOG          log file     (default /tmp/nvpowerd_ioctl.log)
 *   NVIOC_PATCH        0 = dry run  (default 1 = patch)
 *
 * Build:
 *   make
 *   make install
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define NV_ESC_RM_CONTROL  0xc020462au
#define RM_GET_ABS_LIMITS  0x2080a618u
#define RM_SET_POWER_LIMIT 0x2080e634u

#define MAX_DELTA_MW       200000u   /* sanity cap for NVIOC_BOOST_DELTA */

static FILE *g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);

static uint32_t g_delta;             /* mW added to each boost tier (0=stock) */
static int      g_patch = 1;         /* NVIOC_PATCH=0 -> dry run */

/* change tracking */
static uint32_t g_last_limit_applied; /* last effective limit we observed/set */
static int      g_limit_logged;
static int      g_tiers_logged;
static uint32_t g_base, g_max;        /* learned from ABS_LIMITS header */

static void init(void) {
    if (g_log) return;
    const char *path = getenv("NVIOC_LOG");
    if (!path) path = "/tmp/nvpowerd_ioctl.log";
    g_log = fopen(path, "a");
    if (!g_log) g_log = stderr;
    setvbuf(g_log, NULL, _IOLBF, 0);
    real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    real_open   = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    const char *e;
    if ((e = getenv("NVIOC_BOOST_DELTA"))) {
        long v = strtol(e, NULL, 0);
        if (v >= 0 && (unsigned long)v <= MAX_DELTA_MW) g_delta = (uint32_t)v;
    }
    if ((e = getenv("NVIOC_PATCH")) && atoi(e) == 0) g_patch = 0;
    fprintf(g_log, "[nvpowerd-boost] pid=%d delta=%umW patch=%d log=%s\n",
            getpid(), g_delta, g_patch, path);
}

static void ts(char *buf, size_t n) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm; localtime_r(&t.tv_sec, &tm);
    snprintf(buf, n, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000);
}

/* NVOS54 layout: +8 cmd, +16 params pointer, +24 params size */
static int parse_rm(const void *arg, uint32_t *cmd, void **params, uint32_t *psize) {
    const uint8_t *p = arg;
    memcpy(cmd, p + 8, 4);
    uint64_t ptr; uint32_t sz;
    memcpy(&ptr, p + 16, 8);
    memcpy(&sz,  p + 24, 4);
    *params = (void *)ptr;
    *psize  = sz;
    return (*cmd & 0xffff0000u) == 0x20800000u;
}

static int is_power_like(uint32_t v) {
    return v >= 5000 && v <= 400000 && (v % 1000) == 0;
}

/*
 * ABS_LIMITS response: learn base/max from the header (dwords 185/226 in the
 * driver-615 layout) and bump the descending boost-tier quartet.
 */
static void patch_abs_limits(void *params, uint32_t psize) {
    uint32_t *dw = params;
    size_t cnt = psize / 4;
    if (cnt < 230) return;

    uint32_t max = dw[185];
    uint32_t base = dw[226];
    if (is_power_like(max) && is_power_like(base) && base < max) {
        g_max = max;
        g_base = base;
    }

    for (size_t i = 0; i + 3 < cnt; i++) {
        uint32_t a = dw[i], b = dw[i + 1], c = dw[i + 2], d = dw[i + 3];
        if (!is_power_like(a) || !is_power_like(b) ||
            !is_power_like(c) || !is_power_like(d)) continue;
        if (!(a > b && b > c && c > d)) continue;
        if (d > 100000) continue;          /* boost deltas are small */
        if ((a - d) != 30000) continue;    /* stock spacing: 55000-25000 */

        uint32_t before[4] = { a, b, c, d };
        uint32_t after[4]  = { a, b, c, d };
        if (g_delta && g_patch) {
            for (int k = 0; k < 4; k++) {
                after[k] = before[k] + g_delta;
                dw[i + k] = after[k];
            }
        }
        if (!g_tiers_logged) {
            char t[32]; ts(t, sizeof t);
            fprintf(g_log,
                "%s [BOOST] tier deltas %u/%u/%u/%u -> %u/%u/%u/%u mW "
                "(base=%u max=%u requested=+%u%s)\n",
                t, before[0], before[1], before[2], before[3],
                after[0], after[1], after[2], after[3],
                g_base, g_max, g_delta,
                (g_delta == 0 || !g_patch) ? ", unchanged" : "");
            g_tiers_logged = 1;
        }
        return;
    }
}

/*
 * Set-power-limit request: raise base+stock_delta by g_delta and clamp
 * anything above the learned maximum. Log when the effective limit changes.
 */
static void patch_set_limit(void *params, uint32_t psize) {
    uint32_t *dw = params;
    if (psize < 48) return;

    uint32_t seen = dw[11];
    if (!is_power_like(seen)) return;

    uint32_t applied = seen;
    if (g_patch && g_patch && g_base && g_max) {
        if (g_delta && seen == g_base + 25000) {
            applied = g_base + 25000 + g_delta;
        }
        if (applied > g_max) applied = g_max;
        dw[11] = applied;
    }

    if (!g_limit_logged || applied != g_last_limit_applied) {
        char t[32]; ts(t, sizeof t);
        fprintf(g_log, "%s [POWER] limit %u -> %u mW%s%s\n",
                t,
                g_limit_logged ? g_last_limit_applied : seen,
                applied,
                applied != seen ? " (patched)" : "",
                g_patch ? "" : " (DRY-RUN)");
        g_last_limit_applied = applied;
        g_limit_logged = 1;
    }
}

int open(const char *path, int flags, ...) {
    init();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    init();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return real_openat(dirfd, path, flags, mode);
}

int ioctl(int fd, unsigned long request, ...) {
    init();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    uint32_t cmd = 0; void *params = NULL; uint32_t psize = 0;
    int have = (request == NV_ESC_RM_CONTROL) && arg &&
               parse_rm(arg, &cmd, &params, &psize);

    if (have && params && cmd == RM_SET_POWER_LIMIT) {
        pthread_mutex_lock(&g_lock);
        patch_set_limit(params, psize);
        pthread_mutex_unlock(&g_lock);
    }

    int ret = real_ioctl(fd, request, arg);

    if (have && params && psize > 0 && psize <= 0x20000 &&
        cmd == RM_GET_ABS_LIMITS) {
        pthread_mutex_lock(&g_lock);
        patch_abs_limits(params, psize);
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

__attribute__((constructor)) static void ctor(void) { init(); }
