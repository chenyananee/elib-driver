/* test_elib_drv_mt.c - POSIX multi thread check (not part of the default
 * build): a pthread backed port must serialize the device ops, and an
 * exclusive device must admit exactly one concurrent open. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "elib_drv.h"

#define MT_MUTEX_MAX   8
#define MT_THREADS     2
#define MT_IOCTLS      100000
#define MT_CTRL_WRITES 100000
#define MT_OPENERS     8

static pthread_mutex_t g_mutexes[MT_MUTEX_MAX];
static int g_mutex_used[MT_MUTEX_MAX];
static pthread_mutex_t g_guard = PTHREAD_MUTEX_INITIALIZER;

static void *mt_create(const char *name)
{
    int i;

    (void)name;
    pthread_mutex_lock(&g_guard);
    for (i = 0; i < MT_MUTEX_MAX; i++) {
        if (!g_mutex_used[i]) {
            pthread_mutex_init(&g_mutexes[i], NULL);
            g_mutex_used[i] = 1;
            pthread_mutex_unlock(&g_guard);
            return &g_mutexes[i];
        }
    }
    pthread_mutex_unlock(&g_guard);
    return NULL;
}

static void mt_destroy(void *mutex)
{
    int i;

    pthread_mutex_lock(&g_guard);
    for (i = 0; i < MT_MUTEX_MAX; i++) {
        if (mutex == &g_mutexes[i] && g_mutex_used[i]) {
            (void)pthread_mutex_destroy(&g_mutexes[i]);
            g_mutex_used[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_guard);
}

static int mt_lock(void *mutex, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (pthread_mutex_lock((pthread_mutex_t *)mutex) == 0) ? 0 : -1;
}

static int mt_unlock(void *mutex)
{
    return (pthread_mutex_unlock((pthread_mutex_t *)mutex) == 0) ? 0 : -1;
}

static const elib_drv_port_t g_port = { mt_create, mt_destroy, mt_lock,
                                        mt_unlock };

static int g_in_op;
static int g_overlap;
static int g_errors;
static long g_ops;

/* Both the device ioctl path and the direct controller write path touch this
 * fake "hardware"; they must never overlap because they share the controller
 * lock. */
static void hw_touch(void)
{
    if (__atomic_exchange_n(&g_in_op, 1, __ATOMIC_SEQ_CST) != 0) {
        __atomic_store_n(&g_overlap, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&g_ops, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_in_op, 0, __ATOMIC_SEQ_CST);
}

static int mt_ioctl(elib_drv_device_t *dev, uint32_t cmd, void *arg)
{
    elib_drv_controller_t *ctrl = elib_drv_device_controller(dev);
    int rc;

    (void)cmd;
    (void)arg;
    rc = elib_drv_controller_lock(ctrl, ELIB_DRV_WAIT_FOREVER);
    if (rc != 0) {
        return rc;
    }
    hw_touch();
    (void)elib_drv_controller_unlock(ctrl);
    return 0;
}

static int mt_ctrl_write(elib_drv_controller_t *ctrl, const void *buf,
                         size_t len)
{
    (void)ctrl;
    (void)buf;
    hw_touch();
    return (int)len;
}

static const elib_drv_ops_t g_ops_table = {
    NULL, NULL, NULL, NULL, mt_ioctl
};

static const elib_drv_controller_fops_t g_ctrl_fops = {
    .open = NULL,
    .close = NULL,
    .read = NULL,
    .write = mt_ctrl_write,
    .ioctl = NULL,
};

static elib_drv_controller_t g_ctrl_ctx;
static elib_drv_device_t g_mt_ctx;
static elib_drv_device_t g_excl_ctx;

static const elib_drv_controller_desc_t g_ctrls[] = {
    { .name = "bus", .ops = NULL, .fops = &g_ctrl_fops, .priv = NULL,
      .ctx = &g_ctrl_ctx },
};

static const elib_drv_device_desc_t g_devs[] = {
    { .name = "mt", .ops = &g_ops_table, .priv = NULL, .ctx = &g_mt_ctx,
      .controller = "bus" },
    { .name = "excl", .ops = &g_ops_table, .priv = NULL, .ctx = &g_excl_ctx,
      .controller = "bus", .flags = ELIB_DRV_DEV_F_EXCLUSIVE },
};

static const elib_drv_config_t g_cfg = {
    .controllers = g_ctrls,
    .controller_count = ELIB_DRV_ARRAY_SIZE(g_ctrls),
    .devices = g_devs,
    .device_count = ELIB_DRV_ARRAY_SIZE(g_devs),
    .runtime = NULL,
};

static elib_drv_device_t *g_mt_dev;
static elib_drv_device_t *g_excl_dev;
static elib_drv_controller_t *g_bus;
static int g_open_ok;

static void *ioctl_worker(void *arg)
{
    long i;

    (void)arg;
    for (i = 0; i < MT_IOCTLS; i++) {
        if (elib_drv_ioctl(g_mt_dev, 1, NULL) != 0) {
            __atomic_store_n(&g_errors, 1, __ATOMIC_SEQ_CST);
        }
    }
    return NULL;
}

static void *ctrl_writer(void *arg)
{
    long i;

    (void)arg;
    for (i = 0; i < MT_CTRL_WRITES; i++) {
        if (elib_drv_controller_write(g_bus, "x", 1) != 1) {
            __atomic_store_n(&g_errors, 1, __ATOMIC_SEQ_CST);
        }
    }
    return NULL;
}

static void *open_worker(void *arg)
{
    (void)arg;
    if (elib_drv_open(g_excl_dev, 0) == 0) {
        __atomic_add_fetch(&g_open_ok, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[MT_OPENERS];
    int i;

    if (elib_drv_init(&g_port, &g_cfg) != 0) {
        printf("init failed\n");
        return 1;
    }
    g_mt_dev = elib_drv_device_find("mt");
    g_excl_dev = elib_drv_device_find("excl");
    g_bus = elib_drv_controller_find("bus");
    if (g_mt_dev == NULL || g_excl_dev == NULL || g_bus == NULL) {
        printf("device lookup failed\n");
        return 1;
    }
    if (elib_drv_open(g_mt_dev, 0) != 0) {
        printf("open failed\n");
        return 1;
    }
    if (elib_drv_controller_open(g_bus, 0) != 0) {
        printf("controller open failed\n");
        return 1;
    }

    for (i = 0; i < MT_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, ioctl_worker, NULL) != 0) {
            printf("pthread_create failed\n");
            return 1;
        }
    }
    if (pthread_create(&threads[MT_THREADS], NULL, ctrl_writer, NULL) != 0) {
        printf("pthread_create failed\n");
        return 1;
    }
    for (i = 0; i < MT_THREADS + 1; i++) {
        (void)pthread_join(threads[i], NULL);
    }
    (void)elib_drv_close(g_mt_dev);
    if (elib_drv_controller_close(g_bus) != 0) {
        printf("controller close failed\n");
        return 1;
    }

    if (__atomic_load_n(&g_overlap, __ATOMIC_SEQ_CST) != 0) {
        printf("FAIL: device ops overlapped\n");
        return 1;
    }
    if (__atomic_load_n(&g_errors, __ATOMIC_SEQ_CST) != 0) {
        printf("FAIL: ioctl returned an error\n");
        return 1;
    }
    if (__atomic_load_n(&g_ops, __ATOMIC_SEQ_CST) !=
        (long)(MT_THREADS * MT_IOCTLS + MT_CTRL_WRITES)) {
        printf("FAIL: hardware op count mismatch\n");
        return 1;
    }

    for (i = 0; i < MT_OPENERS; i++) {
        if (pthread_create(&threads[i], NULL, open_worker, NULL) != 0) {
            printf("pthread_create failed\n");
            return 1;
        }
    }
    for (i = 0; i < MT_OPENERS; i++) {
        (void)pthread_join(threads[i], NULL);
    }
    if (__atomic_load_n(&g_open_ok, __ATOMIC_SEQ_CST) != 1) {
        printf("FAIL: exclusive open admitted %d callers\n",
               __atomic_load_n(&g_open_ok, __ATOMIC_SEQ_CST));
        return 1;
    }
    if (elib_drv_close(g_excl_dev) != 0) {
        printf("FAIL: close of exclusive device failed\n");
        return 1;
    }

    if (elib_drv_deinit() != 0) {
        printf("FAIL: deinit failed\n");
        return 1;
    }
    printf("MT ALL PASSED (%ld hardware ops, %d exclusive winner)\n",
           __atomic_load_n(&g_ops, __ATOMIC_SEQ_CST),
           __atomic_load_n(&g_open_ok, __ATOMIC_SEQ_CST));
    return 0;
}
