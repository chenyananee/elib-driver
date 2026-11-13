/* Functional regression tests for the unified device-tree driver framework. */
#include "elib_drv.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                         \
    do {                                                                   \
        int a_ = (int)(actual);                                            \
        int e_ = (int)(expected);                                          \
        ++g_checks;                                                        \
        if (a_ != e_) {                                                    \
            ++g_failures;                                                  \
            printf("FAIL %s:%d: %s == %d, expected %d\n",                  \
                   __FILE__, __LINE__, #actual, a_, e_);                   \
        }                                                                  \
    } while (0)

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        (void)elib_drv_deinit();                                           \
        printf("-- %s\n", #fn);                                            \
        fn();                                                              \
    } while (0)

/* ---- fake driver ------------------------------------------------------- */

typedef struct {
    int init_calls;
    int deinit_calls;
    int open_calls;
    int close_calls;
    int read_calls;
    int write_calls;
    int ioctl_calls;
    int init_rc;
    int open_rc;
    int read_rc;
    int write_rc;
    int ioctl_rc;
    int init_order;
    int deinit_order;
    uint32_t last_flags;
    uint32_t last_cmd;
    void *last_arg;
    size_t last_len;
    unsigned char last_write[32];
} fake_ctx_t;

static int g_init_seq;
static int g_deinit_seq;

static fake_ctx_t g_c0, g_d0, g_c1, g_d1, g_fdev;

static void ctx_reset(fake_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void reset_all(void)
{
    ctx_reset(&g_c0);
    ctx_reset(&g_d0);
    ctx_reset(&g_c1);
    ctx_reset(&g_d1);
    ctx_reset(&g_fdev);
    g_init_seq = 0;
    g_deinit_seq = 0;
}

static int fake_init(elib_drv_node_t *node)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->init_calls;
    ctx->init_order = ++g_init_seq;
    return ctx->init_rc;
}

static int fake_deinit(elib_drv_node_t *node)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->deinit_calls;
    ctx->deinit_order = ++g_deinit_seq;
    return 0;
}

static int fake_open(elib_drv_node_t *node, uint32_t flags)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->open_calls;
    ctx->last_flags = flags;
    return ctx->open_rc;
}

static int fake_close(elib_drv_node_t *node)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->close_calls;
    return 0;
}

static int fake_read(elib_drv_node_t *node, void *buf, size_t len)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->read_calls;
    ctx->last_len = len;
    if (ctx->read_rc != 0) {
        return ctx->read_rc;
    }
    if (buf != NULL && len != 0u) {
        memset(buf, 0x5A, len);
    }
    return (int)len;
}

static int fake_write(elib_drv_node_t *node, const void *buf, size_t len)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->write_calls;
    ctx->last_len = len;
    if (ctx->write_rc != 0) {
        return ctx->write_rc;
    }
    if (buf != NULL && len != 0u) {
        size_t copy = len < sizeof(ctx->last_write) ? len
                                                    : sizeof(ctx->last_write);
        memcpy(ctx->last_write, buf, copy);
    }
    return (int)len;
}

static int fake_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg)
{
    fake_ctx_t *ctx = (fake_ctx_t *)elib_drv_node_ctx(node);

    ++ctx->ioctl_calls;
    ctx->last_cmd = cmd;
    ctx->last_arg = arg;
    return ctx->ioctl_rc;
}

static int forward_read(elib_drv_node_t *node, void *buf, size_t len)
{
    elib_drv_node_t *ctrl =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }
    return elib_drv_read(ctrl, buf, len);
}

static int forward_write(elib_drv_node_t *node, const void *buf, size_t len)
{
    elib_drv_node_t *ctrl =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }
    return elib_drv_write(ctrl, buf, len);
}

static const elib_drv_fops_t g_full_fops = {
    fake_init, fake_deinit, fake_open, fake_close,
    fake_read, fake_write, fake_ioctl
};
static const elib_drv_fops_t g_forward_fops = {
    NULL, NULL, NULL, NULL, forward_read, forward_write, NULL
};
static const elib_drv_fops_t g_empty_fops = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* ---- basic tree: c0 -> d0, c1 -> d1 -------------------------------- */

static const elib_drv_dt_node_t g_c0_children[] = {
    { "d0", &g_full_fops, &g_d0, NULL, 0u, 0u, NULL, 0u }
};
static const elib_drv_dt_node_t g_c1_children[] = {
    { "d1", &g_full_fops, &g_d1, NULL, 0u, 0u, NULL, 0u }
};
static const elib_drv_dt_node_t g_basic_roots[] = {
    { "c0", &g_full_fops, &g_c0, NULL, 0u, 0u,
      g_c0_children, ELIB_DRV_ARRAY_SIZE(g_c0_children) },
    { "c1", &g_full_fops, &g_c1, NULL, 0u, 0u,
      g_c1_children, ELIB_DRV_ARRAY_SIZE(g_c1_children) }
};
static const elib_drv_dt_t g_basic_tree = {
    g_basic_roots, ELIB_DRV_ARRAY_SIZE(g_basic_roots)
};

static elib_drv_node_t g_nodes[8];

/* ---- fake port --------------------------------------------------------- */

typedef struct {
    int dummy;
} fake_mutex_t;

static fake_mutex_t g_mutex_obj;
static int g_mutex_create_calls;
static int g_mutex_create_fail;
static int g_mutex_lock_calls;
static int g_mutex_unlock_calls;
static int g_mutex_lock_fail;

static void *fake_mutex_create(const char *name)
{
    (void)name;
    ++g_mutex_create_calls;
    return g_mutex_create_fail ? NULL : (void *)&g_mutex_obj;
}

static void fake_mutex_destroy(void *mutex)
{
    (void)mutex;
}

static int fake_mutex_lock(void *mutex, uint32_t timeout_ms)
{
    (void)mutex;
    (void)timeout_ms;
    ++g_mutex_lock_calls;
    return g_mutex_lock_fail ? -1 : 0;
}

static int fake_mutex_unlock(void *mutex)
{
    (void)mutex;
    ++g_mutex_unlock_calls;
    return 0;
}

static const elib_drv_port_t g_fake_port = {
    fake_mutex_create, fake_mutex_destroy, fake_mutex_lock,
    fake_mutex_unlock
};

static void reset_port(void)
{
    g_mutex_create_calls = 0;
    g_mutex_create_fail = 0;
    g_mutex_lock_calls = 0;
    g_mutex_unlock_calls = 0;
    g_mutex_lock_fail = 0;
}

/* ---- log capture ------------------------------------------------------- */

static char g_log[4096];
static size_t g_log_len;

static void log_clear(void)
{
    g_log[0] = '\0';
    g_log_len = 0u;
}

static int log_capture(const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (g_log_len >= sizeof(g_log) - 1u) {
        return 0;
    }
    room = sizeof(g_log) - 1u - g_log_len;
    va_start(ap, fmt);
    n = vsnprintf(&g_log[g_log_len], room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n < room ? (size_t)n : room;
        g_log_len += add;
    }
    return n;
}

/* ---- foreach callbacks ------------------------------------------------- */

static int foreach_count_cb(elib_drv_node_t *node, void *arg)
{
    (void)node;
    ++*(int *)arg;
    return 0;
}

static int foreach_stop_cb(elib_drv_node_t *node, void *arg)
{
    (void)node;
    ++*(int *)arg;
    return (*(int *)arg >= 2) ? 11 : 0;
}

/* ---- tests ------------------------------------------------------------- */

static void test_dt_node_count(void)
{
    const elib_drv_dt_t empty = { NULL, 0u };
    unsigned count = 99u;
    elib_drv_dt_node_t depth_nodes[9];
    elib_drv_dt_t deep_tree;
    unsigned i;

    CHECK_EQ(elib_drv_dt_node_count(&empty, &count), 0);
    CHECK_EQ(count, 0);
    CHECK_EQ(elib_drv_dt_node_count(&g_basic_tree, &count), 0);
    CHECK_EQ(count, 4);

    CHECK_EQ(elib_drv_dt_node_count(NULL, &count),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_dt_node_count(&empty, NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);

    {
        const elib_drv_dt_node_t bad[] = {
            { "bad", NULL, NULL, NULL, 0u, 0u, NULL, 1u }
        };
        const elib_drv_dt_t bad_tree = { bad, 1u };

        CHECK_EQ(elib_drv_dt_node_count(&bad_tree, &count),
                 -ELIB_DRV_ERR_INVALID_PARAM);
    }

    memset(depth_nodes, 0, sizeof(depth_nodes));
    for (i = 0u; i < 9u; ++i) {
        depth_nodes[i].name = "nested";
        if (i + 1u < 9u) {
            depth_nodes[i].children = &depth_nodes[i + 1u];
            depth_nodes[i].child_count = 1u;
        }
    }
    deep_tree.children = &depth_nodes[0];
    deep_tree.child_count = 1u;
    CHECK_EQ(elib_drv_dt_node_count(&deep_tree, &count),
             -ELIB_DRV_ERR_INVALID_PARAM);
}

static void test_init_basic(void)
{
    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)), 0);
    CHECK(elib_drv_node_find("c0") != NULL);
    CHECK(elib_drv_node_find("d1") != NULL);
    CHECK(elib_drv_node_find("missing") == NULL);
    CHECK(elib_drv_of_find("c0") != NULL);
    CHECK(elib_drv_of_find("missing") == NULL);

    CHECK_EQ(g_c0.init_calls, 1);
    CHECK_EQ(g_d0.init_calls, 1);
    CHECK_EQ(g_c1.init_calls, 1);
    CHECK_EQ(g_d1.init_calls, 1);
    CHECK(g_c0.init_order < g_d0.init_order);
    CHECK(g_c1.init_order < g_d1.init_order);

    {
        elib_drv_node_t *c0 = elib_drv_node_find("c0");
        elib_drv_node_t *d0 = elib_drv_node_find("d0");
        const elib_drv_dt_node_t *dt = elib_drv_of_find("d0");
        unsigned children = 0u;

        CHECK(c0 != NULL && d0 != NULL && dt != NULL);
        CHECK_EQ(strcmp(elib_drv_node_name(c0), "c0"), 0);
        CHECK_EQ(strcmp(elib_drv_node_name(d0), "d0"), 0);
        CHECK(elib_drv_node_ctx(d0) == (void *)&g_d0);
        CHECK(elib_drv_node_controller(d0) == c0);
        CHECK_EQ(strcmp(elib_drv_node_controller_name(d0), "c0"), 0);
        CHECK(elib_drv_node_controller(c0) == NULL);
        CHECK(elib_drv_node_controller_name(c0) == NULL);
        CHECK_EQ(elib_drv_node_child_count(c0), 1);
        CHECK_EQ(elib_drv_node_open_count(d0), 0);
        CHECK(elib_drv_of_config(dt) == (const void *)&g_d0);
        CHECK_EQ(strcmp(elib_drv_of_name(dt), "d0"), 0);
        CHECK(elib_drv_of_controller(dt) == NULL);
        CHECK(elib_drv_of_children(c0->dt, &children) == g_c0_children);
        CHECK_EQ(children, 1);
    }

    {
        int n = 0;
        CHECK_EQ(elib_drv_node_foreach(foreach_count_cb, &n), 0);
        CHECK_EQ(n, 4);
        CHECK_EQ(elib_drv_node_foreach(NULL, NULL),
                 -ELIB_DRV_ERR_INVALID_PARAM);
    }

    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK(elib_drv_node_find("c0") == NULL);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);

    CHECK(g_d0.deinit_order < g_c0.deinit_order);
    CHECK(g_d1.deinit_order < g_c1.deinit_order);
    CHECK_EQ(g_c0.deinit_calls, 1);
    CHECK_EQ(g_d0.deinit_calls, 1);
    CHECK(g_init_seq > 0);
    CHECK(g_deinit_seq > 0);
}

static void test_init_auto_all_and_reinit(void)
{
    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)), 0);
    CHECK_EQ(g_c0.init_calls, 1);
    CHECK_EQ(g_d0.init_calls, 1);
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)),
             -ELIB_DRV_ERR_ALREADY_INIT);
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)), 0);
    CHECK_EQ(g_c0.init_calls, 2);
    CHECK_EQ(g_d0.init_calls, 2);
    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_controller_chain_and_cycle(void)
{
    static fake_ctx_t ca, cb, cc;
    static const elib_drv_dt_node_t chain_nodes[] = {
        { "a", &g_full_fops, &ca, NULL, 0u, 0u, NULL, 0u },
        { "b", &g_full_fops, &cb, "a", 0u, 0u, NULL, 0u },
        { "c", &g_full_fops, &cc, "b", 0u, 0u, NULL, 0u }
    };
    static const elib_drv_dt_t chain_tree = {
        chain_nodes, ELIB_DRV_ARRAY_SIZE(chain_nodes)
    };
    static fake_ctx_t xa, xb;
    static const elib_drv_dt_node_t cycle_nodes[] = {
        { "xa", &g_full_fops, &xa, "xb", 0u, 0u, NULL, 0u },
        { "xb", &g_full_fops, &xb, "xa", 0u, 0u, NULL, 0u }
    };
    static const elib_drv_dt_t cycle_tree = {
        cycle_nodes, ELIB_DRV_ARRAY_SIZE(cycle_nodes)
    };

    ctx_reset(&ca);
    ctx_reset(&cb);
    ctx_reset(&cc);
    CHECK_EQ(elib_drv_init_tree(NULL, &chain_tree, g_nodes, 8u), 0);
    CHECK_EQ(ca.init_calls, 1);
    CHECK_EQ(cb.init_calls, 1);
    CHECK_EQ(cc.init_calls, 1);
    CHECK(ca.init_order < cb.init_order);
    CHECK(cb.init_order < cc.init_order);
    CHECK(elib_drv_node_controller(elib_drv_node_find("b")) ==
          elib_drv_node_find("a"));
    CHECK(elib_drv_node_controller(elib_drv_node_find("c")) ==
          elib_drv_node_find("b"));
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK(cc.deinit_order < cb.deinit_order);
    CHECK(cb.deinit_order < ca.deinit_order);

    ctx_reset(&xa);
    ctx_reset(&xb);
    CHECK_EQ(elib_drv_init_tree(NULL, &cycle_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(xa.init_calls, 0);
    CHECK_EQ(xb.init_calls, 0);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
}

static void test_init_failure_rollback(void)
{
    reset_all();
    g_d0.init_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)),
             -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_c0.init_calls, 1);
    CHECK_EQ(g_d0.init_calls, 1);
    CHECK_EQ(g_d0.deinit_calls, 0);
    CHECK_EQ(g_c0.deinit_calls, 1);
    CHECK(elib_drv_node_find("c0") == NULL);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);

    reset_all();
    g_c0.init_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)),
             -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_c0.init_calls, 1);
    CHECK_EQ(g_d0.init_calls, 0);
    CHECK_EQ(g_c0.deinit_calls, 0);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);

    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_tree_validation(void)
{
    const elib_drv_dt_t empty = { NULL, 0u };
    elib_drv_dt_node_t bad_nodes[2];
    elib_drv_dt_t bad_tree;

    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &empty, NULL, 0u), 0);
    CHECK_EQ(elib_drv_deinit(), 0);

    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 3u),
             -ELIB_DRV_ERR_NO_SLOT);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);

    CHECK_EQ(elib_drv_init_tree(NULL, NULL, g_nodes, 8u),
             -ELIB_DRV_ERR_INVALID_PARAM);

    memset(bad_nodes, 0, sizeof(bad_nodes));
    bad_nodes[0].name = "dup";
    bad_nodes[1].name = "dup";
    bad_tree.children = bad_nodes;
    bad_tree.child_count = 2u;
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_ALREADY_EXIST);

    bad_nodes[0].name = "";
    bad_nodes[1].name = "x";
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_INVALID_PARAM);

    bad_nodes[0].name = "a";
    bad_nodes[1].name = "b";
    bad_nodes[0].controller = "missing";
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_NOT_FOUND);

    bad_nodes[0].controller = "";
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_INVALID_PARAM);

    bad_nodes[0].controller = "a";
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_INVALID_PARAM);

    bad_nodes[0].controller = NULL;
    CHECK_EQ(elib_drv_init_tree(NULL, &bad_tree, g_nodes, 1u),
             -ELIB_DRV_ERR_NO_SLOT);

    {
        elib_drv_port_t bad_port;
        memset(&bad_port, 0, sizeof(bad_port));
        CHECK_EQ(elib_drv_init_tree(&bad_port, &g_basic_tree, g_nodes,
                                    ELIB_DRV_ARRAY_SIZE(g_nodes)),
                 -ELIB_DRV_ERR_INVALID_PARAM);
    }

    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
}

static void test_io_without_open(void)
{
    elib_drv_node_t *d0;
    unsigned char buf[4];
    unsigned char out[4] = { 1u, 2u, 3u, 4u };
    int tag = 7;

    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes,
                                ELIB_DRV_ARRAY_SIZE(g_nodes)), 0);
    d0 = elib_drv_node_find("d0");
    CHECK(d0 != NULL);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(elib_drv_read(d0, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK_EQ(g_d0.read_calls, 1);
    CHECK_EQ(g_d0.last_len, sizeof(buf));
    CHECK_EQ(buf[0], 0x5A);

    CHECK_EQ(elib_drv_write(d0, out, sizeof(out)), (int)sizeof(out));
    CHECK_EQ(g_d0.write_calls, 1);
    CHECK_EQ(g_d0.last_len, sizeof(out));
    CHECK(memcmp(g_d0.last_write, out, sizeof(out)) == 0);

    CHECK_EQ(elib_drv_ioctl(d0, ELIB_DRV_IOC(1u, 2u), &tag), 0);
    CHECK_EQ(g_d0.ioctl_calls, 1);
    CHECK_EQ(g_d0.last_cmd, ELIB_DRV_IOC(1u, 2u));
    CHECK(g_d0.last_arg == (void *)&tag);

    CHECK_EQ(elib_drv_close(d0), -ELIB_DRV_ERR_NOT_OPEN);
    CHECK_EQ(elib_drv_open(d0, 0x11u), 0);
    CHECK_EQ(g_d0.open_calls, 1);
    CHECK_EQ(g_d0.last_flags, 0x11u);
    CHECK_EQ(elib_drv_open(d0, 0), 0);
    CHECK_EQ(g_d0.open_calls, 1);
    CHECK_EQ(elib_drv_node_open_count(d0), 2);
    CHECK_EQ(elib_drv_close(d0), 0);
    CHECK_EQ(elib_drv_close(d0), 0);
    CHECK_EQ(g_d0.close_calls, 1);
    CHECK_EQ(elib_drv_node_open_count(d0), 0);

    g_d0.read_rc = 100;
    CHECK_EQ(elib_drv_read(d0, buf, 1u), -ELIB_DRV_ERR_IO);
    g_d0.read_rc = 0;

    CHECK_EQ(elib_drv_read(NULL, buf, sizeof(buf)),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_read(d0, NULL, 1u),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_write(d0, NULL, 1u),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_read(d0, buf, (size_t)INT_MAX + 1u),
             -ELIB_DRV_ERR_INVALID_PARAM);

    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_exclusive_and_failure(void)
{
    static fake_ctx_t ex_ctx;
    static const elib_drv_dt_node_t ex_node[] = {
        { "ex", &g_full_fops, &ex_ctx, NULL, ELIB_DRV_DEV_F_EXCLUSIVE,
          0u, NULL, 0u }
    };
    static const elib_drv_dt_t ex_tree = { ex_node, 1u };
    elib_drv_node_t *ex;
    unsigned char buf[1];

    ctx_reset(&ex_ctx);
    CHECK_EQ(elib_drv_init_tree(NULL, &ex_tree, g_nodes, 8u), 0);
    ex = elib_drv_node_find("ex");
    CHECK(ex != NULL);
    CHECK_EQ(elib_drv_open(ex, 0), 0);
    CHECK_EQ(elib_drv_open(ex, 0), -ELIB_DRV_ERR_BUSY);
    CHECK_EQ(elib_drv_read(ex, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK_EQ(elib_drv_close(ex), 0);
    CHECK_EQ(elib_drv_open(ex, 0), 0);

    ex_ctx.open_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_close(ex), 0);
    CHECK_EQ(elib_drv_open(ex, 0), -ELIB_DRV_ERR_IO);
    CHECK_EQ(elib_drv_node_open_count(ex), 0);
    ex_ctx.open_rc = 0;
    CHECK_EQ(elib_drv_deinit(), 0);

    reset_all();
    g_d0.open_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 8u), 0);
    {
        elib_drv_node_t *d0 = elib_drv_node_find("d0");
        CHECK_EQ(elib_drv_open(d0, 0), -ELIB_DRV_ERR_IO);
        CHECK_EQ(elib_drv_node_open_count(d0), 0);
        CHECK_EQ(elib_drv_close(d0), -ELIB_DRV_ERR_NOT_OPEN);
    }
    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_missing_fops_and_empty_tree(void)
{
    static fake_ctx_t empty_ctx;
    static const elib_drv_dt_node_t empty_nodes[] = {
        { "none", &g_empty_fops, &empty_ctx, NULL, 0u, 0u, NULL, 0u },
        { "nullfops", NULL, &empty_ctx, NULL, 0u, 0u, NULL, 0u }
    };
    static const elib_drv_dt_t empty_tree = { empty_nodes, 2u };
    elib_drv_node_t *none;
    unsigned char b[1];

    ctx_reset(&empty_ctx);
    CHECK_EQ(elib_drv_init_tree(NULL, &empty_tree, g_nodes, 8u), 0);
    none = elib_drv_node_find("none");
    CHECK(none != NULL);
    CHECK_EQ(elib_drv_read(none, b, sizeof(b)), -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_write(none, b, sizeof(b)), -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_ioctl(none, 0u, NULL), -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_open(none, 0), 0);
    CHECK_EQ(elib_drv_close(none), 0);
    CHECK_EQ(elib_drv_deinit(), 0);

    {
        const elib_drv_dt_t empty = { NULL, 0u };
        CHECK_EQ(elib_drv_init_tree(NULL, &empty, NULL, 0u), 0);
        CHECK(elib_drv_node_find("x") == NULL);
        CHECK_EQ(elib_drv_node_foreach(NULL, NULL),
                 -ELIB_DRV_ERR_INVALID_PARAM);
        CHECK_EQ(elib_drv_deinit(), 0);
    }
}

static void test_controller_forwarding(void)
{
    static fake_ctx_t c2_ctx;
    static fake_ctx_t fdev_ctx;
    static const elib_drv_dt_node_t c2_children[] = {
        { "fdev", &g_forward_fops, &fdev_ctx, "c2", 0u, 0u, NULL, 0u }
    };
    static const elib_drv_dt_node_t c2_nodes[] = {
        { "c2", &g_full_fops, &c2_ctx, NULL, 0u, 0u,
          c2_children, 1u }
    };
    static const elib_drv_dt_t c2_tree = { c2_nodes, 1u };
    elib_drv_node_t *fdev;
    unsigned char buf[3];
    unsigned char out[2] = { 9u, 8u };

    ctx_reset(&c2_ctx);
    ctx_reset(&fdev_ctx);
    CHECK_EQ(elib_drv_init_tree(NULL, &c2_tree, g_nodes, 8u), 0);
    fdev = elib_drv_node_find("fdev");
    CHECK(fdev != NULL);
    CHECK(elib_drv_node_controller(fdev) == elib_drv_node_find("c2"));
    CHECK_EQ(elib_drv_read(fdev, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK_EQ(c2_ctx.read_calls, 1);
    CHECK_EQ(fdev_ctx.read_calls, 0);
    CHECK_EQ(elib_drv_write(fdev, out, sizeof(out)), (int)sizeof(out));
    CHECK_EQ(c2_ctx.write_calls, 1);
    CHECK_EQ(elib_drv_deinit(), 0);

    {
        static const elib_drv_dt_node_t lonely[] = {
            { "lonely", &g_forward_fops, &fdev_ctx, NULL, 0u, 0u, NULL, 0u }
        };
        static const elib_drv_dt_t lonely_tree = { lonely, 1u };

        ctx_reset(&fdev_ctx);
        CHECK_EQ(elib_drv_init_tree(NULL, &lonely_tree, g_nodes, 8u), 0);
        fdev = elib_drv_node_find("lonely");
        CHECK_EQ(elib_drv_read(fdev, buf, sizeof(buf)),
                 -ELIB_DRV_ERR_NOT_FOUND);
        CHECK_EQ(elib_drv_deinit(), 0);
    }
}

static void test_foreach_and_stop(void)
{
    int count = 0;

    reset_all();
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(elib_drv_node_foreach(foreach_count_cb, &count), 0);
    CHECK_EQ(count, 4);

    count = 0;
    CHECK_EQ(elib_drv_node_foreach(foreach_stop_cb, &count), 11);
    CHECK_EQ(count, 2);
    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_port_and_node_lock(void)
{
    elib_drv_node_t *d0;
    unsigned char buf[1];

    reset_all();
    reset_port();
    CHECK_EQ(elib_drv_init_tree(&g_fake_port, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(g_mutex_create_calls, 4);

    d0 = elib_drv_node_find("d0");
    CHECK_EQ(elib_drv_read(d0, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK(g_mutex_lock_calls > 0);
    CHECK_EQ(g_mutex_lock_calls, g_mutex_unlock_calls);

    g_mutex_lock_calls = 0;
    g_mutex_unlock_calls = 0;
    CHECK_EQ(elib_drv_node_lock(d0, 5u), 0);
    CHECK_EQ(elib_drv_node_unlock(d0), 0);
    CHECK_EQ(g_mutex_lock_calls, 1);
    CHECK_EQ(g_mutex_unlock_calls, 1);

    g_mutex_lock_fail = 1;
    CHECK_EQ(elib_drv_read(d0, buf, sizeof(buf)),
             -ELIB_DRV_ERR_LOCK);
    g_mutex_lock_fail = 0;

    CHECK_EQ(elib_drv_deinit(), 0);

    reset_port();
    g_mutex_create_fail = 1;
    CHECK_EQ(elib_drv_init_tree(&g_fake_port, &g_basic_tree, g_nodes, 8u),
             -ELIB_DRV_ERR_LOCK);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);

    reset_port();
    CHECK_EQ(elib_drv_init_tree(&g_fake_port, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
}

static void test_logging(void)
{
#if ELIB_DRV_LOG_ENABLE
    size_t len;
#endif

    reset_all();
    log_clear();
    CHECK_EQ(elib_drv_set_log(log_capture, ELIB_DRV_LOG_NEWLINE_LF), 0);
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
#if ELIB_DRV_LOG_ENABLE
    len = strlen(g_log);
    CHECK(len > 0u);
    CHECK(g_log[len - 1u] == '\n');
    CHECK(strstr(g_log, "init tree") != NULL);
    CHECK(strstr(g_log, "deinit done") != NULL);
#endif

    log_clear();
    CHECK_EQ(elib_drv_set_log(log_capture, ELIB_DRV_LOG_NEWLINE_CRLF), 0);
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
#if ELIB_DRV_LOG_ENABLE
    len = strlen(g_log);
    CHECK(len >= 2u);
    CHECK(g_log[len - 1u] == '\n');
    CHECK(g_log[len - 2u] == '\r');
#endif

    log_clear();
    CHECK_EQ(elib_drv_set_log(log_capture, ELIB_DRV_LOG_NEWLINE_NONE), 0);
    CHECK_EQ(elib_drv_init_tree(NULL, &g_basic_tree, g_nodes, 8u), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
#if ELIB_DRV_LOG_ENABLE
    len = strlen(g_log);
    CHECK(len > 0u);
    CHECK(g_log[len - 1u] != '\n');
#endif

    CHECK_EQ(elib_drv_set_log(NULL, ELIB_DRV_LOG_NEWLINE_LF), 0);
    CHECK_EQ(elib_drv_set_log(NULL, 3u), -ELIB_DRV_ERR_INVALID_PARAM);
}

static void test_api_when_not_initialized(void)
{
    unsigned char b[1];

    (void)elib_drv_deinit();
    CHECK(elib_drv_node_find("x") == NULL);
    CHECK(elib_drv_of_find("x") == NULL);
    CHECK_EQ(elib_drv_read(NULL, b, sizeof(b)),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_open(NULL, 0), -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_node_foreach(NULL, NULL),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
}

int main(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    RUN_TEST(test_dt_node_count);
    RUN_TEST(test_init_basic);
    RUN_TEST(test_init_auto_all_and_reinit);
    RUN_TEST(test_controller_chain_and_cycle);
    RUN_TEST(test_init_failure_rollback);
    RUN_TEST(test_tree_validation);
    RUN_TEST(test_io_without_open);
    RUN_TEST(test_exclusive_and_failure);
    RUN_TEST(test_missing_fops_and_empty_tree);
    RUN_TEST(test_controller_forwarding);
    RUN_TEST(test_foreach_and_stop);
    RUN_TEST(test_port_and_node_lock);
    RUN_TEST(test_logging);
    RUN_TEST(test_api_when_not_initialized);

    if (g_failures == 0) {
        printf("ALL PASSED (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAILED: %d failure(s) out of %d checks\n", g_failures, g_checks);
    return 1;
}
