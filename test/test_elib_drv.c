/* test_elib_drv.c - functional, dual table, flow log and lock discipline.
 *
 * The suite only includes the public header: everything a user can write is
 * exercised through elib_drv.h alone. */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "elib_drv.h"

static int g_failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define CHECK_EQ(actual, expected)                                       \
    do {                                                                 \
        int a_ = (int)(actual);                                          \
        int e_ = (int)(expected);                                        \
        if (a_ != e_) {                                                  \
            printf("FAIL %s:%d: %s == %d, expected %d\n", __FILE__,      \
                   __LINE__, #actual, a_, e_);                           \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define RUN_TEST(fn)                                                     \
    do {                                                                 \
        printf("Test: %s... ", #fn);                                     \
        fn();                                                            \
        printf("done\n");                                                \
    } while (0)

/* ---- fake RTOS port ---------------------------------------------------- */

#define FAKE_TOKEN_MAX 32

static int g_tokens[FAKE_TOKEN_MAX];
static int g_create_calls;
static int g_destroy_calls;
static int g_lock_calls;
static int g_unlock_calls;
static int g_depth;
static int g_max_depth;
static int g_create_fail_at;
static int g_lock_fail;

static void *fake_mutex_create(const char *name)
{
    (void)name;
    g_create_calls++;
    if (g_create_fail_at > 0 && g_create_calls == g_create_fail_at) {
        return NULL;
    }
    return &g_tokens[g_create_calls % FAKE_TOKEN_MAX];
}

static void fake_mutex_destroy(void *mutex)
{
    (void)mutex;
    g_destroy_calls++;
}

static int fake_mutex_lock(void *mutex, uint32_t timeout_ms)
{
    (void)mutex;
    (void)timeout_ms;
    g_lock_calls++;
    if (g_lock_fail) {
        return -1;
    }
    g_depth++;
    if (g_depth > g_max_depth) {
        g_max_depth = g_depth;
    }
    return 0;
}

static int fake_mutex_unlock(void *mutex)
{
    (void)mutex;
    g_unlock_calls++;
    if (g_depth > 0) {
        g_depth--;
    }
    return 0;
}

static const elib_drv_port_t g_port = {
    fake_mutex_create, fake_mutex_destroy, fake_mutex_lock, fake_mutex_unlock
};

/* ---- recording flow log ------------------------------------------------ */

#define LOG_MAX 64
#define LOG_LEN 160

static char g_log_lines[LOG_MAX][LOG_LEN];
static int g_log_count;
static int g_log_max_depth;

static int fake_log(const char *fmt, ...)
{
    va_list ap;

    if (g_depth > g_log_max_depth) {
        g_log_max_depth = g_depth;
    }
    if (g_log_count < LOG_MAX) {
        va_start(ap, fmt);
        (void)vsnprintf(g_log_lines[g_log_count], LOG_LEN, fmt, ap);
        va_end(ap);
        g_log_count++;
    }
    return 0;
}

/* log assertions only make sense when the flow log is compiled in */
#if ELIB_DRV_LOG_ENABLE
#define LOG_TESTS 1
#else
#define LOG_TESTS 0
#endif

#if LOG_TESTS
static int log_has(const char *needle)
{
    int i;

    for (i = 0; i < g_log_count; i++) {
        if (strstr(g_log_lines[i], needle) != NULL) {
            return 1;
        }
    }
    return 0;
}
#else
static int log_has(const char *needle)
{
    (void)needle;
    return 0;
}
#endif

/* ---- fake drivers ------------------------------------------------------ */

typedef struct {
    int init_calls;
    int deinit_calls;
    int init_rc;
    int deinit_rc;
    int open_self_on_init;
    int open_calls;
    int close_calls;
    int read_calls;
    int write_calls;
    int ioctl_calls;
    int open_rc;
    int close_rc;
    int read_rc;
    int write_rc;
    int ioctl_rc;
    int read_extra;
    int last_depth;
    uint32_t last_flags;
    uint32_t last_cmd;
    void *last_arg;
    const void *last_buf;
    size_t last_len;
} fake_ctrl_t;

typedef struct {
    int open_calls;
    int close_calls;
    int read_calls;
    int write_calls;
    int ioctl_calls;
    int open_rc;
    int close_rc;
    int read_rc;
    int write_rc;
    int ioctl_rc;
    int read_extra;
    uint32_t last_flags;
    uint32_t last_cmd;
    void *last_arg;
    const void *last_buf;
    size_t last_len;
} fake_dev_t;

static int ctrl_init(elib_drv_controller_t *ctrl)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->init_calls++;
    if (s->open_self_on_init) {
        int rc = elib_drv_controller_open(ctrl, 0);

        if (rc != 0) {
            return rc;
        }
    }
    return s->init_rc;
}

static int ctrl_deinit(elib_drv_controller_t *ctrl)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->deinit_calls++;
    return s->deinit_rc;
}

static int ctrl_io_open(elib_drv_controller_t *ctrl, uint32_t flags)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->open_calls++;
    s->last_flags = flags;
    s->last_depth = g_depth;
    return s->open_rc;
}

static int ctrl_io_close(elib_drv_controller_t *ctrl)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->close_calls++;
    s->last_depth = g_depth;
    return s->close_rc;
}

static int ctrl_io_read(elib_drv_controller_t *ctrl, void *buf, size_t len)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->read_calls++;
    s->last_buf = buf;
    s->last_len = len;
    s->last_depth = g_depth;
    if (s->read_rc != 0) {
        return s->read_rc;
    }
    if (s->read_extra > 0) {
        return (int)len + s->read_extra;
    }
    return (int)len;
}

static int ctrl_io_write(elib_drv_controller_t *ctrl, const void *buf,
                         size_t len)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->write_calls++;
    s->last_buf = buf;
    s->last_len = len;
    s->last_depth = g_depth;
    if (s->write_rc != 0) {
        return s->write_rc;
    }
    return (int)len;
}

static int ctrl_io_ioctl(elib_drv_controller_t *ctrl, uint32_t cmd, void *arg)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);

    s->ioctl_calls++;
    s->last_cmd = cmd;
    s->last_arg = arg;
    s->last_depth = g_depth;
    return s->ioctl_rc;
}

static const elib_drv_controller_fops_t g_ctrl_fops = {
    ctrl_io_open, ctrl_io_close, ctrl_io_read, ctrl_io_write, ctrl_io_ioctl
};

static int ctrl_init_fail(elib_drv_controller_t *ctrl)
{
    (void)ctrl;
    return -ELIB_DRV_ERR_IO;
}

static const elib_drv_controller_ops_t g_ctrl_ops_fail = {
    ctrl_init_fail, NULL
};

static int dev_open(elib_drv_device_t *dev, uint32_t flags)
{
    fake_dev_t *s = (fake_dev_t *)elib_drv_device_priv(dev);

    s->open_calls++;
    s->last_flags = flags;
    return s->open_rc;
}

static int dev_close(elib_drv_device_t *dev)
{
    fake_dev_t *s = (fake_dev_t *)elib_drv_device_priv(dev);

    s->close_calls++;
    return s->close_rc;
}

static int dev_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    fake_dev_t *s = (fake_dev_t *)elib_drv_device_priv(dev);

    s->read_calls++;
    s->last_buf = buf;
    s->last_len = len;
    if (s->read_rc != 0) {
        return s->read_rc;
    }
    if (s->read_extra > 0) {
        return (int)len + s->read_extra;
    }
    return (int)len;
}

static int dev_write(elib_drv_device_t *dev, const void *buf, size_t len)
{
    fake_dev_t *s = (fake_dev_t *)elib_drv_device_priv(dev);

    s->write_calls++;
    s->last_buf = buf;
    s->last_len = len;
    if (s->write_rc != 0) {
        return s->write_rc;
    }
    return (int)len;
}

static int dev_ioctl(elib_drv_device_t *dev, uint32_t cmd, void *arg)
{
    fake_dev_t *s = (fake_dev_t *)elib_drv_device_priv(dev);

    s->ioctl_calls++;
    s->last_cmd = cmd;
    s->last_arg = arg;
    return s->ioctl_rc;
}

/* ---- static configuration ---------------------------------------------- */

static elib_drv_controller_t g_ctrl_ctx[4];
static elib_drv_device_t g_dev_ctx[6];
static elib_drv_controller_slot_t g_rt_ctrl[2];
static elib_drv_device_slot_t g_rt_dev[4];

static fake_ctrl_t g_ctrl_state;
static fake_ctrl_t g_rt_ctrl_state;
static fake_dev_t g_dev_state[3];
static fake_dev_t g_rt_state;

static const elib_drv_controller_ops_t g_ctrl_ops = { ctrl_init, ctrl_deinit };
static const elib_drv_ops_t g_dev_ops = {
    dev_open, dev_close, dev_read, dev_write, dev_ioctl
};

static const elib_drv_controller_desc_t g_ctrls[] = {
    { .name = "i2c1", .ops = &g_ctrl_ops, .fops = &g_ctrl_fops,
      .priv = &g_ctrl_state, .ctx = &g_ctrl_ctx[0] },
    { .name = "i2c2", .ops = NULL, .priv = NULL, .ctx = &g_ctrl_ctx[1] },
};

static const elib_drv_device_desc_t g_devs[] = {
    { .name = "eeprom", .ops = &g_dev_ops, .priv = &g_dev_state[0],
      .ctx = &g_dev_ctx[0], .controller = "i2c1" },
    { .name = "adc", .ops = NULL, .priv = &g_dev_state[1],
      .ctx = &g_dev_ctx[1], .controller = NULL },
    { .name = "sensor", .ops = &g_dev_ops, .priv = &g_dev_state[2],
      .ctx = &g_dev_ctx[2], .controller = "i2c2" },
};

static const elib_drv_runtime_t g_runtime = {
    .controllers = { g_rt_ctrl, sizeof g_rt_ctrl },
    .devices = { g_rt_dev, sizeof g_rt_dev },
};

/* rollback config: the first controller opens itself in init, the second one
 * fails, so the init failure must close the first controller again */
static const elib_drv_controller_desc_t g_rollback_ctrls[] = {
    { .name = "open_self", .ops = &g_ctrl_ops, .fops = &g_ctrl_fops,
      .priv = &g_rt_ctrl_state, .ctx = &g_ctrl_ctx[2] },
    { .name = "fail", .ops = &g_ctrl_ops_fail, .priv = NULL,
      .ctx = &g_ctrl_ctx[3] },
};

static const elib_drv_config_t g_cfg = {
    .controllers = g_ctrls,
    .controller_count = ELIB_DRV_ARRAY_SIZE(g_ctrls),
    .devices = g_devs,
    .device_count = ELIB_DRV_ARRAY_SIZE(g_devs),
    .runtime = &g_runtime,
};

/* ---- harness ----------------------------------------------------------- */

static void harness_reset(void)
{
    memset(&g_ctrl_state, 0, sizeof g_ctrl_state);
    memset(&g_rt_ctrl_state, 0, sizeof g_rt_ctrl_state);
    memset(g_dev_state, 0, sizeof g_dev_state);
    memset(&g_rt_state, 0, sizeof g_rt_state);
    memset(g_ctrl_ctx, 0, sizeof g_ctrl_ctx);
    memset(g_dev_ctx, 0, sizeof g_dev_ctx);
    memset(g_rt_ctrl, 0, sizeof g_rt_ctrl);
    memset(g_rt_dev, 0, sizeof g_rt_dev);
    memset(g_log_lines, 0, sizeof g_log_lines);

    g_create_calls = 0;
    g_destroy_calls = 0;
    g_lock_calls = 0;
    g_unlock_calls = 0;
    g_depth = 0;
    g_max_depth = 0;
    g_create_fail_at = 0;
    g_lock_fail = 0;
    g_log_count = 0;
    g_log_max_depth = 0;

    (void)elib_drv_set_log(fake_log);
}

static void harness_begin(void)
{
    int rc = elib_drv_deinit();

    CHECK(rc == 0 || rc == -ELIB_DRV_ERR_NOT_INITIALIZED);
    harness_reset();
}

static void harness_end(void)
{
    int rc = elib_drv_deinit();

    CHECK(rc == 0 || rc == -ELIB_DRV_ERR_NOT_INITIALIZED);
}

static elib_drv_config_t cfg_no_runtime(void)
{
    elib_drv_config_t cfg = g_cfg;

    cfg.runtime = NULL;
    return cfg;
}

static elib_drv_device_desc_t make_dev_desc(const char *name, fake_dev_t *state,
                                            const char *controller)
{
    elib_drv_device_desc_t desc;

    memset(&desc, 0, sizeof desc);
    desc.name = name;
    desc.ops = &g_dev_ops;
    desc.priv = state;
    desc.controller = controller;
    return desc;
}

/* A controller whose init hook registers a child device: used to check that a
 * failing init rolls its children back together with the controller slot. */
static int ctrl_init_with_child(elib_drv_controller_t *ctrl)
{
    fake_ctrl_t *s = (fake_ctrl_t *)elib_drv_controller_priv(ctrl);
    elib_drv_device_desc_t desc = make_dev_desc(
        "child_dev", &g_rt_state, elib_drv_controller_name(ctrl));

    s->init_calls++;
    if (elib_drv_register_device(&desc, NULL) != 0) {
        return -ELIB_DRV_ERR_IO;
    }
    return s->init_rc;
}

static const elib_drv_controller_ops_t g_ctrl_ops_with_child = {
    ctrl_init_with_child, ctrl_deinit
};

static int count_devices(elib_drv_device_t *dev, void *arg)
{
    int *count = (int *)arg;

    (void)dev;
    (*count)++;
    return 0;
}

static int stop_after_two(elib_drv_device_t *dev, void *arg)
{
    int *count = (int *)arg;

    (void)dev;
    (*count)++;
    return (*count == 2) ? 42 : 0;
}

/* ---- init / deinit ----------------------------------------------------- */

static void test_init_wires_static(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK(g_ctrl_ctx[0].desc == &g_ctrls[0]);
    CHECK(g_ctrl_ctx[0].device_count == 1);
    CHECK(g_ctrl_ctx[1].device_count == 1);
    CHECK(g_dev_ctx[0].controller == &g_ctrl_ctx[0]);
    CHECK(g_dev_ctx[1].controller == NULL);
    CHECK(strcmp(elib_drv_device_name(&g_dev_ctx[0]), "eeprom") == 0);
    CHECK_EQ((int)elib_drv_device_open_count(&g_dev_ctx[0]), 0);
    CHECK_EQ(g_create_calls, 5);
    CHECK_EQ(g_ctrl_state.init_calls, 1);
    CHECK(!LOG_TESTS || log_has("init ok"));
    harness_end();
}

static void test_init_null_port(void)
{
    unsigned char buf[4];
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(NULL, &g_cfg), 0);
    CHECK_EQ(g_create_calls, 0);
    dev = elib_drv_device_find("eeprom");
    CHECK(dev != NULL);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), 4);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_init_invalid_config(void)
{
    elib_drv_config_t cfg;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, NULL), -ELIB_DRV_ERR_INVALID_PARAM);

    cfg = g_cfg;
    cfg.controllers = NULL;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_INVALID_PARAM);

    cfg = g_cfg;
    cfg.devices = NULL;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_INVALID_PARAM);

    cfg = g_cfg;
    cfg.controller_count = 0;
    cfg.devices = NULL;
    cfg.device_count = 0;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), 0);
    CHECK(elib_drv_device_find("eeprom") == NULL);
    CHECK_EQ(elib_drv_deinit(), 0);
    harness_end();
}

static void test_init_bad_port(void)
{
    static const elib_drv_port_t bad = {
        fake_mutex_create, NULL, fake_mutex_lock, fake_mutex_unlock
    };

    harness_begin();
    CHECK_EQ(elib_drv_init(&bad, &g_cfg), -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

static const elib_drv_controller_desc_t g_dup_ctrls[] = {
    { .name = "a", .ops = NULL, .priv = NULL, .ctx = &g_ctrl_ctx[0] },
    { .name = "a", .ops = NULL, .priv = NULL, .ctx = &g_ctrl_ctx[1] },
};

static const elib_drv_device_desc_t g_dup_devs[] = {
    { .name = "x", .ops = NULL, .priv = NULL, .ctx = &g_dev_ctx[0],
      .controller = NULL },
    { .name = "x", .ops = NULL, .priv = NULL, .ctx = &g_dev_ctx[1],
      .controller = NULL },
};

static const elib_drv_controller_desc_t g_null_ctx_ctrls[] = {
    { .name = "a", .ops = NULL, .priv = NULL, .ctx = NULL },
};

static const elib_drv_device_desc_t g_unknown_ctrl_devs[] = {
    { .name = "x", .ops = NULL, .priv = NULL, .ctx = &g_dev_ctx[0],
      .controller = "nope" },
};

static void test_init_duplicate_names(void)
{
    elib_drv_config_t cfg;

    harness_begin();
    cfg = g_cfg;
    cfg.controllers = g_dup_ctrls;
    cfg.controller_count = ELIB_DRV_ARRAY_SIZE(g_dup_ctrls);
    cfg.devices = NULL;
    cfg.device_count = 0;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_ALREADY_EXIST);

    cfg = g_cfg;
    cfg.controllers = NULL;
    cfg.controller_count = 0;
    cfg.devices = g_dup_devs;
    cfg.device_count = ELIB_DRV_ARRAY_SIZE(g_dup_devs);
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_ALREADY_EXIST);
    harness_end();
}

static void test_init_null_ctx(void)
{
    elib_drv_config_t cfg;

    harness_begin();
    cfg = g_cfg;
    cfg.controllers = g_null_ctx_ctrls;
    cfg.controller_count = ELIB_DRV_ARRAY_SIZE(g_null_ctx_ctrls);
    cfg.devices = NULL;
    cfg.device_count = 0;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

static void test_init_unknown_controller(void)
{
    elib_drv_config_t cfg;

    harness_begin();
    cfg = g_cfg;
    cfg.devices = g_unknown_ctrl_devs;
    cfg.device_count = ELIB_DRV_ARRAY_SIZE(g_unknown_ctrl_devs);
    cfg.controllers = NULL;
    cfg.controller_count = 0;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_NOT_FOUND);
    harness_end();
}

static void test_init_twice(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), -ELIB_DRV_ERR_ALREADY_INIT);
    CHECK_EQ(g_create_calls, 5);
    harness_end();
}

static void test_api_before_init(void)
{
    unsigned char buf[4];

    harness_begin();
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK(elib_drv_controller_find("i2c1") == NULL);
    CHECK(elib_drv_device_find("eeprom") == NULL);
    CHECK_EQ(elib_drv_device_foreach(count_devices, NULL),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_open(&g_dev_ctx[0], 0), -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_close(&g_dev_ctx[0]), -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_read(&g_dev_ctx[0], buf, 1),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_write(&g_dev_ctx[0], buf, 1),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_ioctl(&g_dev_ctx[0], 1, NULL),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_register_controller(&g_ctrls[0], NULL),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_register_device(&g_devs[0], NULL),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_unregister_device(&g_dev_ctx[0]),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_controller_lock(&g_ctrl_ctx[0], 0),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK_EQ(elib_drv_controller_unlock(&g_ctrl_ctx[0]),
             -ELIB_DRV_ERR_NOT_INITIALIZED);
    CHECK(elib_drv_controller_name(&g_ctrl_ctx[0]) == NULL);
    CHECK(elib_drv_device_name(&g_dev_ctx[0]) == NULL);
    harness_end();
}

static void test_deinit_busy(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_BUSY);
    CHECK(g_dev_ctx[0].desc != NULL);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 1);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    harness_end();
}

static void test_deinit_destroys_locks(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK_EQ(g_create_calls, 5);
    CHECK_EQ(g_destroy_calls, 5);
    CHECK(g_dev_ctx[0].desc == NULL);
    CHECK(g_ctrl_ctx[0].desc == NULL);
    harness_end();
}

static void test_deinit_reports_controller_error(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    g_ctrl_state.deinit_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_create_calls, g_destroy_calls);
    CHECK(g_dev_ctx[0].desc == NULL);
    CHECK(g_ctrl_ctx[0].desc == NULL);
    g_ctrl_state.deinit_rc = 0;
    harness_end();
}

static void test_lock_create_failure_rolls_back(void)
{
    harness_begin();
    g_create_fail_at = 3;
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), -ELIB_DRV_ERR_LOCK);
    CHECK_EQ(g_create_calls, 3);
    CHECK_EQ(g_destroy_calls, 2);
    CHECK(g_ctrl_ctx[0].desc == NULL);
    CHECK(g_dev_ctx[0].desc == NULL);
    CHECK_EQ(g_ctrl_state.init_calls, 0);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
    g_create_fail_at = 0;
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    harness_end();
}

static void test_controller_init_failure_rolls_back(void)
{
    harness_begin();
    g_ctrl_state.init_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_ctrl_state.init_calls, 1);
    CHECK_EQ(g_ctrl_state.deinit_calls, 0);
    CHECK_EQ(g_create_calls, 5);
    CHECK_EQ(g_destroy_calls, 5);
    CHECK(g_ctrl_ctx[0].desc == NULL);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
    g_ctrl_state.init_rc = 0;
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    harness_end();
}

/* ---- lookup / iteration ------------------------------------------------ */

static void test_find(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK(elib_drv_controller_find("i2c1") == &g_ctrl_ctx[0]);
    CHECK(elib_drv_controller_find("i2c2") == &g_ctrl_ctx[1]);
    CHECK(elib_drv_controller_find("nope") == NULL);
    CHECK(elib_drv_controller_find(NULL) == NULL);
    CHECK(elib_drv_device_find("eeprom") == &g_dev_ctx[0]);
    CHECK(elib_drv_device_find("nope") == NULL);
    CHECK(elib_drv_device_find(NULL) == NULL);
    harness_end();
}

static void test_foreach(void)
{
    int count = 0;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_device_foreach(count_devices, &count), 0);
    CHECK_EQ(count, 3);

    /* runtime devices are appended after the static ones */
    {
        elib_drv_device_desc_t desc = make_dev_desc("rt", &g_rt_state, NULL);

        CHECK_EQ(elib_drv_register_device(&desc, NULL), 0);
        count = 0;
        CHECK_EQ(elib_drv_device_foreach(count_devices, &count), 0);
        CHECK_EQ(count, 4);
    }
    count = 0;
    CHECK_EQ(elib_drv_device_foreach(stop_after_two, &count), 42);
    CHECK_EQ(count, 2);
    CHECK_EQ(elib_drv_device_foreach(NULL, NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

/* ---- open / close ------------------------------------------------------ */

static void test_open_close_refcount(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(g_dev_state[0].open_calls, 1);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 3);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(g_dev_state[0].close_calls, 1);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 0);
    CHECK_EQ(elib_drv_close(dev), -ELIB_DRV_ERR_NOT_OPEN);

    /* a failing close still releases the count */
    g_dev_state[0].close_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), -ELIB_DRV_ERR_IO);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 0);
    g_dev_state[0].close_rc = 0;
    harness_end();
}

static void test_open_exclusive(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("excl", &g_rt_state, NULL);
    elib_drv_device_t *dev = NULL;

    desc.flags = ELIB_DRV_DEV_F_EXCLUSIVE;
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK(dev != NULL);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_open(dev, 0), -ELIB_DRV_ERR_BUSY);
    CHECK_EQ(g_rt_state.open_calls, 1);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_open_failure(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    g_dev_state[0].open_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_open(dev, 0), -ELIB_DRV_ERR_IO);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 0);
    CHECK_EQ(elib_drv_close(dev), -ELIB_DRV_ERR_NOT_OPEN);
    g_dev_state[0].open_rc = 0;
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

/* ---- read / write / ioctl ---------------------------------------------- */

static void test_read_requires_open(void)
{
    unsigned char buf[4];
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), -ELIB_DRV_ERR_NOT_OPEN);
    CHECK_EQ(elib_drv_write(dev, buf, sizeof buf), -ELIB_DRV_ERR_NOT_OPEN);
    CHECK_EQ(elib_drv_ioctl(dev, 1, NULL), -ELIB_DRV_ERR_NOT_OPEN);
    harness_end();
}

static void test_read_argument_checks(void)
{
    unsigned char buf[4];
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_read(dev, NULL, 4), -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_read(dev, buf, (size_t)INT_MAX + 1),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_read(dev, NULL, 0), 0);
    CHECK_EQ(elib_drv_write(dev, NULL, 2), -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_read_result_checks(void)
{
    unsigned char buf[4];
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);

    g_dev_state[0].read_extra = 1;
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), -ELIB_DRV_ERR_IO);
    g_dev_state[0].read_extra = 0;

    g_dev_state[0].read_rc = -ELIB_DRV_ERR_TIMEOUT;
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), -ELIB_DRV_ERR_TIMEOUT);

    g_dev_state[0].read_rc = -ELIB_DRV_ERR_DRIVER_BASE - 7;
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf),
             -ELIB_DRV_ERR_DRIVER_BASE - 7);

    g_dev_state[0].read_rc = 0;
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), 4);
    CHECK(g_dev_state[0].last_buf == buf);
    CHECK_EQ((int)g_dev_state[0].last_len, 4);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_write_and_ioctl(void)
{
    unsigned char buf[3] = { 1, 2, 3 };
    int arg = 5;
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_write(dev, buf, sizeof buf), 3);
    CHECK_EQ(g_dev_state[0].write_calls, 1);
    CHECK(g_dev_state[0].last_buf == buf);
    CHECK_EQ(elib_drv_ioctl(dev, ELIB_DRV_IOC(2, 3), &arg), 0);
    CHECK_EQ((int)g_dev_state[0].last_cmd, (int)ELIB_DRV_IOC(2, 3));
    CHECK(g_dev_state[0].last_arg == &arg);
    g_dev_state[0].ioctl_rc = 7;
    CHECK_EQ(elib_drv_ioctl(dev, 1, NULL), 7);
    g_dev_state[0].ioctl_rc = 0;
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_not_supported(void)
{
    unsigned char buf[2];
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("adc");
    CHECK(dev != NULL);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_write(dev, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_ioctl(dev, 1, NULL), -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_io_not_logged(void)
{
    unsigned char buf[4];
    elib_drv_device_t *dev;
    int before;
    int i;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    before = g_log_count;
    for (i = 0; i < 100; i++) {
        (void)elib_drv_read(dev, buf, sizeof buf);
        (void)elib_drv_ioctl(dev, 1, NULL);
    }
    CHECK_EQ(g_log_count, before);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_lock_timeout(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    g_lock_fail = 1;
    CHECK_EQ(elib_drv_open(dev, 0), -ELIB_DRV_ERR_LOCK);
    CHECK_EQ((int)elib_drv_device_open_count(dev), 0);
    CHECK_EQ(g_depth, 0);
    g_lock_fail = 0;
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    harness_end();
}

static void test_controller_lock(void)
{
    elib_drv_device_t *dev;
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    ctrl = elib_drv_device_controller(dev);
    CHECK(ctrl == &g_ctrl_ctx[0]);
    CHECK_EQ(elib_drv_controller_lock(ctrl, 10), 0);
    CHECK_EQ(g_depth, 1);
    CHECK_EQ(elib_drv_controller_unlock(ctrl), 0);
    CHECK_EQ(g_depth, 0);
    CHECK_EQ(elib_drv_controller_lock(NULL, 10),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_controller_unlock(NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

/* ---- controller direct access ------------------------------------------ */

static void test_controller_direct_refcount(void)
{
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK(ctrl != NULL);
    CHECK_EQ(elib_drv_controller_open(ctrl, 7), 0);
    CHECK_EQ(elib_drv_controller_open(ctrl, 7), 0);
    CHECK_EQ(g_ctrl_state.open_calls, 1);
    CHECK_EQ((int)g_ctrl_state.last_flags, 7);
    CHECK_EQ((int)elib_drv_controller_open_count(ctrl), 2);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ(g_ctrl_state.close_calls, 1);
    CHECK_EQ(elib_drv_controller_close(ctrl), -ELIB_DRV_ERR_NOT_OPEN);
    harness_end();
}

static void test_controller_direct_io(void)
{
    static const char payload[3] = { 'a', 'b', 'c' };
    elib_drv_controller_t *ctrl;
    unsigned char buf[4];
    int arg = 9;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_controller_write(ctrl, payload, sizeof payload), 3);
    CHECK_EQ(g_ctrl_state.write_calls, 1);
    CHECK(g_ctrl_state.last_buf == payload);
    CHECK_EQ((int)g_ctrl_state.last_len, 3);
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf), 4);
    CHECK(g_ctrl_state.last_buf == buf);
    CHECK_EQ((int)g_ctrl_state.last_len, 4);
    g_ctrl_state.ioctl_rc = 5;
    CHECK_EQ(elib_drv_controller_ioctl(ctrl, ELIB_DRV_IOC(4, 2), &arg), 5);
    CHECK_EQ((int)g_ctrl_state.last_cmd, (int)ELIB_DRV_IOC(4, 2));
    CHECK(g_ctrl_state.last_arg == &arg);
    g_ctrl_state.ioctl_rc = 0;
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    harness_end();
}

static void test_controller_direct_requires_open(void)
{
    elib_drv_controller_t *ctrl;
    unsigned char buf[2];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_OPEN);
    CHECK_EQ(elib_drv_controller_write(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_OPEN);
    CHECK_EQ(elib_drv_controller_ioctl(ctrl, 1, NULL),
             -ELIB_DRV_ERR_NOT_OPEN);
    harness_end();
}

static void test_controller_direct_no_fops(void)
{
    elib_drv_controller_t *ctrl;
    unsigned char buf[2];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c2");
    CHECK(ctrl != NULL);
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_controller_write(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_controller_ioctl(ctrl, 1, NULL),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    harness_end();
}

static void test_controller_direct_argument_checks(void)
{
    elib_drv_controller_t *ctrl;
    unsigned char buf[4];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_controller_read(ctrl, NULL, 4),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_controller_write(ctrl, NULL, 4),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, (size_t)INT_MAX + 1),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_controller_read(ctrl, NULL, 0), 0);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    harness_end();
}

static void test_controller_direct_result_checks(void)
{
    elib_drv_controller_t *ctrl;
    unsigned char buf[4];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    g_ctrl_state.read_extra = 1;
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_IO);
    g_ctrl_state.read_extra = 0;
    g_ctrl_state.read_rc = -ELIB_DRV_ERR_TIMEOUT;
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_TIMEOUT);
    g_ctrl_state.read_rc = 0;
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    harness_end();
}

static void test_controller_direct_lock_timeout(void)
{
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    g_lock_fail = 1;
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), -ELIB_DRV_ERR_LOCK);
    CHECK_EQ((int)elib_drv_controller_open_count(ctrl), 0);
    CHECK_EQ(g_depth, 0);
    g_lock_fail = 0;
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    harness_end();
}

static void test_controller_direct_holds_lock(void)
{
    elib_drv_controller_t *ctrl;
    unsigned char buf[2];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(g_ctrl_state.last_depth, 1);
    g_ctrl_state.read_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_controller_read(ctrl, buf, sizeof buf),
             -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_ctrl_state.last_depth, 1);
    CHECK_EQ(g_depth, 0);
    g_ctrl_state.read_rc = 0;
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ(g_ctrl_state.last_depth, 1);
    CHECK_EQ(g_depth, 0);
    harness_end();
}

static void test_controller_deinit_busy_on_open(void)
{
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_BUSY);
    CHECK(ctrl->desc != NULL);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    harness_end();
}

static void test_controller_unregister_busy_on_open(void)
{
    elib_drv_controller_desc_t desc;
    elib_drv_controller_t *ctrl = NULL;

    memset(&desc, 0, sizeof desc);
    desc.name = "rtc";
    desc.fops = &g_ctrl_fops;
    desc.priv = &g_rt_ctrl_state;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_controller(&desc, &ctrl), 0);
    CHECK(ctrl != NULL);
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), -ELIB_DRV_ERR_BUSY);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), 0);
    harness_end();
}

static void test_controller_direct_rollback(void)
{
    elib_drv_config_t cfg = g_cfg;
    int created;
    int destroyed;

    cfg.controllers = g_rollback_ctrls;
    cfg.controller_count = ELIB_DRV_ARRAY_SIZE(g_rollback_ctrls);
    cfg.devices = NULL;
    cfg.device_count = 0;
    cfg.runtime = NULL;

    harness_begin();
    g_rt_ctrl_state.open_self_on_init = 1;
    created = g_create_calls;
    destroyed = g_destroy_calls;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), -ELIB_DRV_ERR_IO);
    CHECK_EQ(g_rt_ctrl_state.open_calls, 1);
    CHECK_EQ(g_rt_ctrl_state.close_calls, 1);
    CHECK(g_ctrl_ctx[2].desc == NULL);
    CHECK(g_ctrl_ctx[3].desc == NULL);
    CHECK_EQ(g_create_calls, created + 2);
    CHECK_EQ(g_destroy_calls, destroyed + 2);
    CHECK_EQ(elib_drv_deinit(), -ELIB_DRV_ERR_NOT_INITIALIZED);
    harness_end();
}

static void test_controller_accessors(void)
{
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ((int)elib_drv_controller_open_count(ctrl), 0);
    CHECK_EQ((int)elib_drv_controller_device_count(ctrl), 1);
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ((int)elib_drv_controller_open_count(ctrl), 1);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK_EQ((int)elib_drv_controller_open_count(ctrl), 0);
    CHECK_EQ((int)elib_drv_controller_open_count(NULL), 0);
    CHECK_EQ((int)elib_drv_controller_device_count(NULL), 0);
    harness_end();
}

/* ---- runtime table ----------------------------------------------------- */

static void test_register_without_runtime(void)
{
    elib_drv_config_t cfg = cfg_no_runtime();
    elib_drv_device_desc_t dev_desc = make_dev_desc("rt", &g_rt_state, NULL);
    elib_drv_controller_desc_t ctrl_desc;

    memset(&ctrl_desc, 0, sizeof ctrl_desc);
    ctrl_desc.name = "newc";
    ctrl_desc.ops = &g_ctrl_ops;
    ctrl_desc.priv = &g_rt_ctrl_state;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &cfg), 0);
    CHECK_EQ(elib_drv_register_device(&dev_desc, NULL),
             -ELIB_DRV_ERR_NO_SLOT);
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, NULL),
             -ELIB_DRV_ERR_NO_SLOT);
    harness_end();
}

static void test_register_runtime_device(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("rtdev", &g_rt_state, "i2c1");
    elib_drv_device_t *dev = NULL;
    unsigned char buf[2];

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK(dev != NULL);
    CHECK(elib_drv_device_find("rtdev") == dev);
    CHECK(elib_drv_device_controller(dev) == &g_ctrl_ctx[0]);
    CHECK_EQ((int)g_ctrl_ctx[0].device_count, 2);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_read(dev, buf, sizeof buf), 2);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_unregister_device(dev), 0);
    CHECK(elib_drv_device_find("rtdev") == NULL);
    CHECK_EQ((int)g_ctrl_ctx[0].device_count, 1);
    harness_end();
}

static void test_register_duplicates(void)
{
    elib_drv_device_desc_t desc;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);

    desc = make_dev_desc("eeprom", &g_rt_state, NULL);
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_ALREADY_EXIST);

    desc = make_dev_desc("tmp", &g_rt_state, NULL);
    CHECK_EQ(elib_drv_register_device(&desc, NULL), 0);
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_ALREADY_EXIST);

    desc = make_dev_desc(NULL, &g_rt_state, NULL);
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_register_device(NULL, NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

static void test_register_unknown_controller(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("orphan", &g_rt_state, "nope");

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_NOT_FOUND);
    CHECK(elib_drv_device_find("orphan") == NULL);
    harness_end();
}

static void test_runtime_controller_lifecycle(void)
{
    elib_drv_controller_desc_t ctrl_desc;
    elib_drv_device_desc_t dev_desc;
    elib_drv_controller_t *ctrl = NULL;
    elib_drv_device_t *dev = NULL;

    memset(&ctrl_desc, 0, sizeof ctrl_desc);
    ctrl_desc.name = "dyn";
    ctrl_desc.ops = &g_ctrl_ops;
    ctrl_desc.priv = &g_rt_ctrl_state;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, &ctrl), 0);
    CHECK(ctrl != NULL);
    CHECK_EQ(g_rt_ctrl_state.init_calls, 1);
    CHECK(elib_drv_controller_find("dyn") == ctrl);
    CHECK_EQ((int)ctrl->device_count, 0);

    dev_desc = make_dev_desc("dyn_dev", &g_rt_state, "dyn");
    CHECK_EQ(elib_drv_register_device(&dev_desc, &dev), 0);
    CHECK(elib_drv_device_controller(dev) == ctrl);
    CHECK_EQ((int)ctrl->device_count, 1);

    CHECK_EQ(elib_drv_unregister_controller(ctrl), -ELIB_DRV_ERR_BUSY);
    CHECK_EQ(elib_drv_unregister_device(dev), 0);
    CHECK_EQ((int)ctrl->device_count, 0);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), 0);
    CHECK_EQ(g_rt_ctrl_state.deinit_calls, 1);
    CHECK(elib_drv_controller_find("dyn") == NULL);

    g_rt_ctrl_state.init_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, NULL),
             -ELIB_DRV_ERR_IO);
    CHECK(elib_drv_controller_find("dyn") == NULL);

    /* a failing deinit is reported but the slot is still released */
    g_rt_ctrl_state.init_rc = 0;
    g_rt_ctrl_state.deinit_rc = -ELIB_DRV_ERR_IO;
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, &ctrl), 0);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), -ELIB_DRV_ERR_IO);
    CHECK(elib_drv_controller_find("dyn") == NULL);
    harness_end();
}

static void test_runtime_controller_init_cleans_children(void)
{
    elib_drv_controller_desc_t ctrl_desc;
    elib_drv_controller_t *ctrl = NULL;
    elib_drv_device_t *child;
    int created;
    int destroyed;

    memset(&ctrl_desc, 0, sizeof ctrl_desc);
    ctrl_desc.name = "parent";
    ctrl_desc.ops = &g_ctrl_ops_with_child;
    ctrl_desc.priv = &g_rt_ctrl_state;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);

    /* successful init: the child exists, is bound and blocks unregistration */
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, &ctrl), 0);
    CHECK(ctrl != NULL);
    child = elib_drv_device_find("child_dev");
    CHECK(child != NULL);
    CHECK(elib_drv_device_controller(child) == ctrl);
    CHECK_EQ((int)ctrl->device_count, 1);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), -ELIB_DRV_ERR_BUSY);
    CHECK_EQ(elib_drv_unregister_device(child), 0);
    CHECK_EQ(elib_drv_unregister_controller(ctrl), 0);
    CHECK(elib_drv_device_find("child_dev") == NULL);

    /* failing init: the child registered during init is rolled back too */
    g_rt_ctrl_state.init_rc = -ELIB_DRV_ERR_IO;
    created = g_create_calls;
    destroyed = g_destroy_calls;
    CHECK_EQ(elib_drv_register_controller(&ctrl_desc, NULL),
             -ELIB_DRV_ERR_IO);
    CHECK(elib_drv_device_find("child_dev") == NULL);
    CHECK(elib_drv_controller_find("parent") == NULL);
    CHECK_EQ(g_create_calls, created + 2);
    CHECK_EQ(g_destroy_calls, destroyed + 2);
    harness_end();
}

static void test_runtime_device_slots_exhausted(void)
{
    static const char *const names[] = { "r0", "r1", "r2", "r3", "r4" };
    elib_drv_device_desc_t desc = make_dev_desc("r0", &g_rt_state, NULL);
    unsigned i;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    for (i = 0; i < 4; i++) {
        desc.name = names[i];
        CHECK_EQ(elib_drv_register_device(&desc, NULL), 0);
    }
    desc.name = names[4];
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_NO_SLOT);
    CHECK(elib_drv_device_find(names[4]) == NULL);
    CHECK(!LOG_TESTS || log_has("register failed"));
    harness_end();
}

static void test_unregister_open_device_busy(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("rt", &g_rt_state, NULL);
    elib_drv_device_t *dev = NULL;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_unregister_device(dev), -ELIB_DRV_ERR_BUSY);
    CHECK(elib_drv_device_find("rt") == dev);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(elib_drv_unregister_device(dev), 0);
    harness_end();
}

static void test_unregister_static_not_supported(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_unregister_device(&g_dev_ctx[0]),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_unregister_controller(&g_ctrl_ctx[0]),
             -ELIB_DRV_ERR_NOT_SUPPORTED);
    CHECK_EQ(elib_drv_unregister_device(NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    CHECK_EQ(elib_drv_unregister_controller(NULL),
             -ELIB_DRV_ERR_INVALID_PARAM);
    harness_end();
}

static void test_unregister_then_reuse_slot(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("tmp", &g_rt_state, NULL);
    elib_drv_device_t *dev = NULL;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK_EQ(elib_drv_unregister_device(dev), 0);
    CHECK(elib_drv_device_find("tmp") == NULL);
    desc.name = "tmp2";
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK(dev != NULL);
    CHECK(elib_drv_device_find("tmp2") == dev);
    harness_end();
}

static unsigned char g_tiny_mem[1];
static unsigned char g_raw_mem[1024];

static void test_runtime_tiny_and_unaligned(void)
{
    elib_drv_runtime_t runtime;
    elib_drv_config_t cfg = g_cfg;
    elib_drv_device_desc_t desc = make_dev_desc("rt", &g_rt_state, NULL);
    elib_drv_device_t *dev = NULL;

    runtime.controllers.mem = g_tiny_mem;
    runtime.controllers.bytes = sizeof g_tiny_mem;
    runtime.devices.mem = g_tiny_mem;
    runtime.devices.bytes = sizeof g_tiny_mem;
    cfg.runtime = &runtime;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, NULL), -ELIB_DRV_ERR_NO_SLOT);
    CHECK_EQ(elib_drv_deinit(), 0);

    runtime.controllers.mem = g_raw_mem + 1;
    runtime.controllers.bytes = 400;
    runtime.devices.mem = g_raw_mem + 401;
    runtime.devices.bytes = 600;
    CHECK_EQ(elib_drv_init(&g_port, &cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, &dev), 0);
    CHECK(dev != NULL);
    CHECK_EQ(elib_drv_unregister_device(dev), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    harness_end();
}

/* ---- log --------------------------------------------------------------- */

#if LOG_TESTS

static void test_log_flow(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK(log_has("init (static c=2 d=3 runtime c=2 d=4)"));
    CHECK(log_has("init ok"));
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK(log_has("eeprom open"));
    CHECK(log_has("eeprom close"));
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK(log_has("deinit (static c=2 d=3 runtime c=2 d=4)"));
    CHECK_EQ(g_log_max_depth, 0);
    harness_end();
}

static void test_log_disabled(void)
{
    elib_drv_device_t *dev;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK(g_log_count > 0);
    (void)elib_drv_set_log(NULL);
    g_log_count = 0;
    dev = elib_drv_device_find("eeprom");
    CHECK_EQ(elib_drv_open(dev, 0), 0);
    CHECK_EQ(elib_drv_close(dev), 0);
    CHECK_EQ(g_log_count, 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK_EQ(g_log_count, 0);
    harness_end();
}

static void test_log_survives_deinit(void)
{
    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_deinit(), 0);
    CHECK(log_has("deinit"));
    g_log_count = 0;
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK(log_has("init ok"));
    CHECK_EQ(elib_drv_deinit(), 0);
    harness_end();
}

static void test_log_register_failures(void)
{
    elib_drv_device_desc_t desc = make_dev_desc("eeprom", &g_rt_state, NULL);
    elib_drv_device_desc_t orphan =
        make_dev_desc("orphan", &g_rt_state, "nope");

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    CHECK_EQ(elib_drv_register_device(&desc, NULL),
             -ELIB_DRV_ERR_ALREADY_EXIST);
    CHECK(log_has("dev eeprom register failed"));
    CHECK_EQ(elib_drv_register_device(&orphan, NULL),
             -ELIB_DRV_ERR_NOT_FOUND);
    CHECK(log_has("dev orphan register failed"));
    harness_end();
}

static void test_controller_direct_log(void)
{
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    ctrl = elib_drv_controller_find("i2c1");
    CHECK_EQ(elib_drv_controller_open(ctrl, 0), 0);
    CHECK_EQ(elib_drv_controller_close(ctrl), 0);
    CHECK(log_has("ctrl i2c1 open"));
    CHECK(log_has("ctrl i2c1 close"));
    CHECK_EQ(g_log_max_depth, 0);
    harness_end();
}

#endif /* LOG_TESTS */

/* ---- accessors / layout ------------------------------------------------ */

static void test_accessors(void)
{
    elib_drv_device_t *dev;
    elib_drv_controller_t *ctrl;

    harness_begin();
    CHECK_EQ(elib_drv_init(&g_port, &g_cfg), 0);
    dev = elib_drv_device_find("eeprom");
    ctrl = elib_drv_controller_find("i2c1");
    CHECK(strcmp(elib_drv_device_name(dev), "eeprom") == 0);
    CHECK(elib_drv_device_priv(dev) == &g_dev_state[0]);
    CHECK(strcmp(elib_drv_controller_name(ctrl), "i2c1") == 0);
    CHECK(elib_drv_controller_priv(ctrl) == &g_ctrl_state);
    CHECK(elib_drv_device_controller(dev) == ctrl);
    CHECK(elib_drv_controller_name(NULL) == NULL);
    CHECK(elib_drv_controller_priv(NULL) == NULL);
    CHECK(elib_drv_device_name(NULL) == NULL);
    CHECK(elib_drv_device_priv(NULL) == NULL);
    CHECK(elib_drv_device_controller(NULL) == NULL);
    CHECK_EQ((int)elib_drv_device_open_count(NULL), 0);
    harness_end();
}

static void test_ctx_sizes(void)
{
    CHECK(sizeof(elib_drv_device_t) <= 3 * sizeof(void *) + 8);
    CHECK(sizeof(elib_drv_controller_t) <= 2 * sizeof(void *) + 8);
    CHECK(sizeof(elib_drv_device_slot_t) > sizeof(elib_drv_device_t));
}

/* ---- main -------------------------------------------------------------- */

int main(void)
{
    printf("=== elib-driver tests ===\n");

    RUN_TEST(test_init_wires_static);
    RUN_TEST(test_init_null_port);
    RUN_TEST(test_init_invalid_config);
    RUN_TEST(test_init_bad_port);
    RUN_TEST(test_init_duplicate_names);
    RUN_TEST(test_init_null_ctx);
    RUN_TEST(test_init_unknown_controller);
    RUN_TEST(test_init_twice);
    RUN_TEST(test_api_before_init);
    RUN_TEST(test_deinit_busy);
    RUN_TEST(test_deinit_destroys_locks);
    RUN_TEST(test_deinit_reports_controller_error);
    RUN_TEST(test_lock_create_failure_rolls_back);
    RUN_TEST(test_controller_init_failure_rolls_back);
    RUN_TEST(test_find);
    RUN_TEST(test_foreach);
    RUN_TEST(test_open_close_refcount);
    RUN_TEST(test_open_exclusive);
    RUN_TEST(test_open_failure);
    RUN_TEST(test_read_requires_open);
    RUN_TEST(test_read_argument_checks);
    RUN_TEST(test_read_result_checks);
    RUN_TEST(test_write_and_ioctl);
    RUN_TEST(test_not_supported);
    RUN_TEST(test_io_not_logged);
    RUN_TEST(test_lock_timeout);
    RUN_TEST(test_controller_lock);
    RUN_TEST(test_controller_direct_refcount);
    RUN_TEST(test_controller_direct_io);
    RUN_TEST(test_controller_direct_requires_open);
    RUN_TEST(test_controller_direct_no_fops);
    RUN_TEST(test_controller_direct_argument_checks);
    RUN_TEST(test_controller_direct_result_checks);
    RUN_TEST(test_controller_direct_lock_timeout);
    RUN_TEST(test_controller_direct_holds_lock);
    RUN_TEST(test_controller_deinit_busy_on_open);
    RUN_TEST(test_controller_unregister_busy_on_open);
    RUN_TEST(test_controller_direct_rollback);
    RUN_TEST(test_controller_accessors);
    RUN_TEST(test_register_without_runtime);
    RUN_TEST(test_register_runtime_device);
    RUN_TEST(test_register_duplicates);
    RUN_TEST(test_register_unknown_controller);
    RUN_TEST(test_runtime_controller_lifecycle);
    RUN_TEST(test_runtime_controller_init_cleans_children);
    RUN_TEST(test_runtime_device_slots_exhausted);
    RUN_TEST(test_unregister_open_device_busy);
    RUN_TEST(test_unregister_static_not_supported);
    RUN_TEST(test_unregister_then_reuse_slot);
    RUN_TEST(test_runtime_tiny_and_unaligned);
#if LOG_TESTS
    RUN_TEST(test_log_flow);
    RUN_TEST(test_log_disabled);
    RUN_TEST(test_log_survives_deinit);
    RUN_TEST(test_log_register_failures);
    RUN_TEST(test_controller_direct_log);
#endif
    RUN_TEST(test_accessors);
    RUN_TEST(test_ctx_sizes);

    printf("\n%s (%d failures)\n", (g_failures == 0) ? "ALL PASSED" : "FAILED",
           g_failures);
    return (g_failures == 0) ? 0 : 1;
}
