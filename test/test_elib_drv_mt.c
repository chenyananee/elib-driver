/* test_elib_drv_mt.c - POSIX multi thread check (not part of the default
 * build): the controller node lock must serialize device I/O and direct
 * controller I/O, and an exclusive node must admit one concurrent open. */
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
static int g_open_ok;

static void hw_touch(void)
{
    if (__atomic_exchange_n(&g_in_op, 1, __ATOMIC_SEQ_CST) != 0) {
        __atomic_store_n(&g_overlap, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&g_ops, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_in_op, 0, __ATOMIC_SEQ_CST);
}

static int bus_write(elib_drv_node_t *node, const void *buf, size_t len)
{
    (void)node;
    (void)buf;
    hw_touch();
    return (int)len;
}

static int mt_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg)
{
    elib_drv_node_t *bus =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    (void)cmd;
    (void)arg;
    if (bus == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }
    return elib_drv_write(bus, "x", 1u);
}

static const elib_drv_fops_t g_bus_fops = {
    NULL, NULL, NULL, NULL, NULL, bus_write, NULL
};
static const elib_drv_fops_t g_mt_fops = {
    NULL, NULL, NULL, NULL, NULL, NULL, mt_ioctl
};
static const elib_drv_fops_t g_excl_fops = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static const elib_drv_dt_node_t g_bus_children[] = {
    { "mt", &g_mt_fops, NULL, NULL, 0u, 0u, NULL, 0u },
    { "excl", &g_excl_fops, NULL, NULL, ELIB_DRV_DEV_F_EXCLUSIVE,
      0u, NULL, 0u }
};
static const elib_drv_dt_node_t g_roots[] = {
    { "bus", &g_bus_fops, NULL, NULL, 0u, 0u,
      g_bus_children, ELIB_DRV_ARRAY_SIZE(g_bus_children) }
};
static const elib_drv_dt_t g_tree = {
    g_roots, ELIB_DRV_ARRAY_SIZE(g_roots)
};

static elib_drv_node_t g_nodes[3];
static elib_drv_node_t *g_mt_dev;
static elib_drv_node_t *g_excl_dev;
static elib_drv_node_t *g_bus;

static void *ioctl_worker(void *arg)
{
    long i;

    (void)arg;
    for (i = 0; i < MT_IOCTLS; i++) {
        if (elib_drv_ioctl(g_mt_dev, 1u, NULL) != 0) {
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
        if (elib_drv_write(g_bus, "x", 1u) != 1) {
            __atomic_store_n(&g_errors, 1, __ATOMIC_SEQ_CST);
        }
    }
    return NULL;
}

static void *open_worker(void *arg)
{
    (void)arg;
    if (elib_drv_open(g_excl_dev, 0u) == 0) {
        __atomic_add_fetch(&g_open_ok, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[MT_OPENERS];
    int i;

    if (elib_drv_init_tree(&g_port, &g_tree, g_nodes,
                           ELIB_DRV_ARRAY_SIZE(g_nodes)) != 0) {
        printf("init failed\n");
        return 1;
    }
    g_mt_dev = elib_drv_node_find("mt");
    g_excl_dev = elib_drv_node_find("excl");
    g_bus = elib_drv_node_find("bus");
    if (g_mt_dev == NULL || g_excl_dev == NULL || g_bus == NULL) {
        printf("node lookup failed\n");
        return 1;
    }

    if (elib_drv_open(g_mt_dev, 0u) != 0) {
        printf("open failed\n");
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

    if (__atomic_load_n(&g_overlap, __ATOMIC_SEQ_CST) != 0) {
        printf("FAIL: bus operations overlapped\n");
        return 1;
    }
    if (__atomic_load_n(&g_errors, __ATOMIC_SEQ_CST) != 0) {
        printf("FAIL: I/O returned an error\n");
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
        printf("FAIL: close of exclusive node failed\n");
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
