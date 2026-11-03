/* elib_drv_manager.c - manager: config validation, static/runtime tables,
 * registration, lookup, lifecycle and the flow log. */
#include "elib_drv_internal.h"

#include <string.h>

elib_drv_manager_t elib_drv_g_manager;

/* ---- lock helpers ------------------------------------------------------ */

int elib_drv__lock(void *lock, uint32_t timeout_ms)
{
    const elib_drv_port_t *port = elib_drv_g_manager.port;

    if (lock == NULL || port == NULL) {
        return 0;
    }
    return (port->mutex_lock(lock, timeout_ms) == 0) ? 0 : -ELIB_DRV_ERR_LOCK;
}

void elib_drv__unlock(void *lock)
{
    const elib_drv_port_t *port = elib_drv_g_manager.port;

    if (lock != NULL && port != NULL) {
        (void)port->mutex_unlock(lock);
    }
}

/* ---- small helpers ----------------------------------------------------- */

static int name_bad(const char *name)
{
    return (name == NULL) || (name[0] == '\0');
}

static elib_drv_controller_t *ctrl_find_static(const elib_drv_manager_t *m,
                                               const char *name)
{
    unsigned i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < m->controller_count; i++) {
        if (strcmp(m->controllers[i].name, name) == 0) {
            return m->controllers[i].ctx;
        }
    }
    return NULL;
}

static elib_drv_controller_t *ctrl_find_any(const elib_drv_manager_t *m,
                                            const char *name)
{
    elib_drv_controller_t *ctx;
    unsigned i;

    ctx = ctrl_find_static(m, name);
    if (ctx != NULL || name == NULL) {
        return ctx;
    }
    for (i = 0; i < m->ctrl_capacity; i++) {
        elib_drv_controller_slot_t *slot = &m->ctrl_slots[i];

        if (slot->ctx.desc != NULL &&
            strcmp(slot->ctx.desc->name, name) == 0) {
            return &slot->ctx;
        }
    }
    return NULL;
}

static elib_drv_device_t *dev_find_any(const elib_drv_manager_t *m,
                                       const char *name)
{
    unsigned i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < m->device_count; i++) {
        if (strcmp(m->devices[i].name, name) == 0) {
            return m->devices[i].ctx;
        }
    }
    for (i = 0; i < m->dev_capacity; i++) {
        elib_drv_device_slot_t *slot = &m->dev_slots[i];

        if (slot->ctx.desc != NULL &&
            strcmp(slot->ctx.desc->name, name) == 0) {
            return &slot->ctx;
        }
    }
    return NULL;
}

/* Derive a slot array from a user supplied byte range: align the base to a
 * pointer boundary, keep the whole number of slots that fit, clear them. */
static void *pool_base(void *mem, size_t bytes, size_t slot_size,
                       unsigned *capacity)
{
    uintptr_t addr;
    uintptr_t aligned;
    size_t pad;
    size_t avail;
    unsigned cap;

    *capacity = 0;
    if (mem == NULL || bytes == 0 || slot_size == 0) {
        return NULL;
    }
    addr = (uintptr_t)mem;
    aligned = (addr + (uintptr_t)(sizeof(void *) - 1)) &
              ~(uintptr_t)(sizeof(void *) - 1);
    pad = (size_t)(aligned - addr);
    if (pad >= bytes) {
        return NULL;
    }
    avail = bytes - pad;
    cap = (unsigned)(avail / slot_size);
    if (cap == 0) {
        return NULL;
    }
    (void)memset((void *)aligned, 0, (size_t)cap * slot_size);
    *capacity = cap;
    return (void *)aligned;
}

static int port_valid(const elib_drv_port_t *port)
{
    if (port == NULL) {
        return 1;
    }
    return (port->mutex_create != NULL) && (port->mutex_destroy != NULL) &&
           (port->mutex_lock != NULL) && (port->mutex_unlock != NULL);
}

static int config_valid(const elib_drv_config_t *cfg)
{
    unsigned i;
    unsigned j;

    if (cfg == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (cfg->controller_count > 0 && cfg->controllers == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (cfg->device_count > 0 && cfg->devices == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (cfg->runtime != NULL) {
        if (cfg->runtime->controllers.mem == NULL &&
            cfg->runtime->controllers.bytes > 0) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        if (cfg->runtime->devices.mem == NULL &&
            cfg->runtime->devices.bytes > 0) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
    }
    for (i = 0; i < cfg->controller_count; i++) {
        const elib_drv_controller_desc_t *ctrl = &cfg->controllers[i];

        if (name_bad(ctrl->name) || ctrl->ctx == NULL) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(cfg->controllers[j].name, ctrl->name) == 0) {
                return -ELIB_DRV_ERR_ALREADY_EXIST;
            }
        }
    }
    for (i = 0; i < cfg->device_count; i++) {
        const elib_drv_device_desc_t *dev = &cfg->devices[i];

        if (name_bad(dev->name) || dev->ctx == NULL) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(cfg->devices[j].name, dev->name) == 0) {
                return -ELIB_DRV_ERR_ALREADY_EXIST;
            }
        }
        if (dev->controller != NULL) {
            int found = 0;

            for (j = 0; j < cfg->controller_count; j++) {
                if (strcmp(cfg->controllers[j].name, dev->controller) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                return -ELIB_DRV_ERR_NOT_FOUND;
            }
        }
    }
    return 0;
}

/* ---- teardown ---------------------------------------------------------- */

static void destroy_lock(const elib_drv_manager_t *m, void **lock)
{
    if (*lock != NULL && m->port != NULL) {
        m->port->mutex_destroy(*lock);
        *lock = NULL;
    }
}

static void release_device(const elib_drv_manager_t *m, elib_drv_device_t *ctx)
{
    const elib_drv_ops_t *ops = (ctx->desc != NULL) ? ctx->desc->ops : NULL;

    if (ctx->open_count != 0 && ops != NULL && ops->close != NULL) {
        (void)ops->close(ctx);
    }
    destroy_lock(m, &ctx->lock);
    (void)memset(ctx, 0, sizeof(*ctx));
}

/* Rollback path only: a normal deinit refuses while a direct open exists.
 * Release it before the controller slot disappears. */
static void close_controller_direct(elib_drv_controller_t *ctx)
{
    const elib_drv_controller_fops_t *fops =
        (ctx->desc != NULL) ? ctx->desc->fops : NULL;

    if (ctx->open_count != 0 && fops != NULL && fops->close != NULL) {
        (void)fops->close(ctx);
    }
    ctx->open_count = 0;
}

static int release_controller(const elib_drv_manager_t *m,
                              elib_drv_controller_t *ctx)
{
    const elib_drv_controller_ops_t *ops =
        (ctx->desc != NULL) ? ctx->desc->ops : NULL;
    int rc = 0;

    close_controller_direct(ctx);
    if (ops != NULL && ops->deinit != NULL) {
        rc = ops->deinit(ctx);
    }
    destroy_lock(m, &ctx->lock);
    (void)memset(ctx, 0, sizeof(*ctx));
    return rc;
}

/* A controller that is being discarded may already own runtime devices (its
 * init hook is allowed to register children before failing). Release them
 * first so no descriptor keeps pointing at a freed slot. */
static void release_bound_devices(elib_drv_manager_t *m,
                                  elib_drv_controller_t *ctrl)
{
    unsigned i;

    for (i = 0; i < m->dev_capacity; i++) {
        elib_drv_device_slot_t *slot = &m->dev_slots[i];

        if (slot->ctx.desc != NULL && slot->ctx.controller == ctrl) {
            release_device(m, &slot->ctx);
            (void)memset(slot, 0, sizeof(*slot));
        }
    }
}

/* Release everything: devices first, then runtime controllers, then static
 * controllers whose init stage was reached. Returns the first deinit error. */
static int teardown(elib_drv_manager_t *m, unsigned inited_static_ctrls)
{
    elib_drv_log_fn log = m->log;
    unsigned i;
    int first = 0;
    int rc;

    for (i = 0; i < m->device_count; i++) {
        release_device(m, m->devices[i].ctx);
    }
    for (i = 0; i < m->dev_capacity; i++) {
        if (m->dev_slots[i].ctx.desc != NULL) {
            release_device(m, &m->dev_slots[i].ctx);
            (void)memset(&m->dev_slots[i], 0, sizeof(m->dev_slots[i]));
        }
    }
    for (i = 0; i < m->ctrl_capacity; i++) {
        if (m->ctrl_slots[i].ctx.desc != NULL) {
            rc = release_controller(m, &m->ctrl_slots[i].ctx);
            if (rc < 0 && first == 0) {
                first = rc;
            }
            (void)memset(&m->ctrl_slots[i], 0, sizeof(m->ctrl_slots[i]));
        }
    }
    for (i = 0; i < m->controller_count; i++) {
        elib_drv_controller_t *ctx = m->controllers[i].ctx;

        if (i < inited_static_ctrls) {
            rc = release_controller(m, ctx);
            if (rc < 0 && first == 0) {
                first = rc;
            }
        } else {
            destroy_lock(m, &ctx->lock);
            (void)memset(ctx, 0, sizeof(*ctx));
        }
    }
    (void)memset(m, 0, sizeof(*m));
    m->log = log;
    return first;
}

/* ---- init / deinit ----------------------------------------------------- */

int elib_drv_init(const elib_drv_port_t *port, const elib_drv_config_t *config)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    elib_drv_log_fn log = m->log;
    int rc;
    unsigned i;
    unsigned inited = 0;

    if (m->initialized) {
        return -ELIB_DRV_ERR_ALREADY_INIT;
    }
    if (!port_valid(port)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = config_valid(config);
    if (rc != 0) {
        return rc;
    }

    (void)memset(m, 0, sizeof(*m));
    m->log = log;
    m->port = port;
    m->controllers = config->controllers;
    m->controller_count = config->controller_count;
    m->devices = config->devices;
    m->device_count = config->device_count;

    if (config->runtime != NULL) {
        m->ctrl_slots = (elib_drv_controller_slot_t *)pool_base(
            config->runtime->controllers.mem,
            config->runtime->controllers.bytes,
            sizeof(elib_drv_controller_slot_t), &m->ctrl_capacity);
        m->dev_slots = (elib_drv_device_slot_t *)pool_base(
            config->runtime->devices.mem,
            config->runtime->devices.bytes,
            sizeof(elib_drv_device_slot_t), &m->dev_capacity);
    }

    ELIB_DRV_LOG("[elib_drv] I: init (static c=%u d=%u runtime c=%u d=%u)",
                 m->controller_count, m->device_count, m->ctrl_capacity,
                 m->dev_capacity);

    for (i = 0; i < m->controller_count; i++) {
        elib_drv_controller_t *ctx = m->controllers[i].ctx;

        (void)memset(ctx, 0, sizeof(*ctx));
        ctx->desc = &m->controllers[i];
        ctx->lock = NULL;
        ctx->device_count = 0;
        ctx->open_count = 0;
    }
    for (i = 0; i < m->device_count; i++) {
        elib_drv_device_t *ctx = m->devices[i].ctx;

        (void)memset(ctx, 0, sizeof(*ctx));
        ctx->desc = &m->devices[i];
        ctx->lock = NULL;
        ctx->controller = (m->devices[i].controller != NULL)
                              ? ctrl_find_static(m, m->devices[i].controller)
                              : NULL;
        ctx->open_count = 0;
    }
    for (i = 0; i < m->device_count; i++) {
        elib_drv_device_t *ctx = m->devices[i].ctx;

        if (ctx->controller != NULL) {
            ctx->controller->device_count++;
        }
    }

    for (i = 0; i < m->controller_count; i++) {
        elib_drv_controller_t *ctx = m->controllers[i].ctx;

        if (m->port != NULL) {
            ctx->lock = m->port->mutex_create(ctx->desc->name);
            if (ctx->lock == NULL) {
                (void)teardown(m, 0);
                ELIB_DRV_LOG("[elib_drv] E: init failed (%d)",
                             -ELIB_DRV_ERR_LOCK);
                return -ELIB_DRV_ERR_LOCK;
            }
        }
    }
    for (i = 0; i < m->device_count; i++) {
        elib_drv_device_t *ctx = m->devices[i].ctx;

        if (m->port != NULL) {
            ctx->lock = m->port->mutex_create(ctx->desc->name);
            if (ctx->lock == NULL) {
                (void)teardown(m, 0);
                ELIB_DRV_LOG("[elib_drv] E: init failed (%d)",
                             -ELIB_DRV_ERR_LOCK);
                return -ELIB_DRV_ERR_LOCK;
            }
        }
    }

    m->initialized = 1;

    for (i = 0; i < m->controller_count; i++) {
        elib_drv_controller_t *ctx = m->controllers[i].ctx;
        const elib_drv_controller_ops_t *ops = ctx->desc->ops;

        if (ops != NULL && ops->init != NULL) {
            rc = ops->init(ctx);
            if (rc < 0) {
                ELIB_DRV_LOG("[elib_drv] E: ctrl %s init failed (%d)",
                             ctx->desc->name, rc);
                (void)teardown(m, inited);
                return rc;
            }
        }
        inited++;
    }

    ELIB_DRV_LOG("[elib_drv] I: init ok");
    return 0;
}

int elib_drv_deinit(void)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    unsigned i;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    for (i = 0; i < m->device_count; i++) {
        if (m->devices[i].ctx->open_count != 0) {
            ELIB_DRV_LOG("[elib_drv] W: deinit busy");
            return -ELIB_DRV_ERR_BUSY;
        }
    }
    for (i = 0; i < m->dev_capacity; i++) {
        if (m->dev_slots[i].ctx.desc != NULL &&
            m->dev_slots[i].ctx.open_count != 0) {
            ELIB_DRV_LOG("[elib_drv] W: deinit busy");
            return -ELIB_DRV_ERR_BUSY;
        }
    }
    for (i = 0; i < m->controller_count; i++) {
        if (m->controllers[i].ctx->open_count != 0) {
            ELIB_DRV_LOG("[elib_drv] W: deinit busy");
            return -ELIB_DRV_ERR_BUSY;
        }
    }
    for (i = 0; i < m->ctrl_capacity; i++) {
        if (m->ctrl_slots[i].ctx.desc != NULL &&
            m->ctrl_slots[i].ctx.open_count != 0) {
            ELIB_DRV_LOG("[elib_drv] W: deinit busy");
            return -ELIB_DRV_ERR_BUSY;
        }
    }

    ELIB_DRV_LOG("[elib_drv] I: deinit (static c=%u d=%u runtime c=%u d=%u)",
                 m->controller_count, m->device_count, m->ctrl_capacity,
                 m->dev_capacity);
    return teardown(m, m->controller_count);
}

/* ---- lookup / iteration ------------------------------------------------ */

elib_drv_controller_t *elib_drv_controller_find(const char *name)
{
    if (!elib_drv_g_manager.initialized) {
        return NULL;
    }
    return ctrl_find_any(&elib_drv_g_manager, name);
}

elib_drv_device_t *elib_drv_device_find(const char *name)
{
    if (!elib_drv_g_manager.initialized) {
        return NULL;
    }
    return dev_find_any(&elib_drv_g_manager, name);
}

int elib_drv_device_foreach(int (*fn)(elib_drv_device_t *dev, void *arg),
                            void *arg)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    unsigned i;
    int rc;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (fn == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    for (i = 0; i < m->device_count; i++) {
        rc = fn(m->devices[i].ctx, arg);
        if (rc != 0) {
            return rc;
        }
    }
    for (i = 0; i < m->dev_capacity; i++) {
        if (m->dev_slots[i].ctx.desc != NULL) {
            rc = fn(&m->dev_slots[i].ctx, arg);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/* ---- flow log ---------------------------------------------------------- */

int elib_drv_set_log(elib_drv_log_fn fn)
{
    elib_drv_g_manager.log = fn;
    return 0;
}

/* ---- runtime registration ---------------------------------------------- */

static elib_drv_controller_slot_t *ctrl_slot_alloc(elib_drv_manager_t *m)
{
    unsigned i;

    for (i = 0; i < m->ctrl_capacity; i++) {
        if (m->ctrl_slots[i].ctx.desc == NULL) {
            return &m->ctrl_slots[i];
        }
    }
    return NULL;
}

static elib_drv_device_slot_t *dev_slot_alloc(elib_drv_manager_t *m)
{
    unsigned i;

    for (i = 0; i < m->dev_capacity; i++) {
        if (m->dev_slots[i].ctx.desc == NULL) {
            return &m->dev_slots[i];
        }
    }
    return NULL;
}

int elib_drv_register_controller(const elib_drv_controller_desc_t *desc,
                                 elib_drv_controller_t **out)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    elib_drv_controller_slot_t *slot;
    int rc;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (desc == NULL || name_bad(desc->name)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (ctrl_find_any(m, desc->name) != NULL) {
        ELIB_DRV_LOG("[elib_drv] W: ctrl %s register failed (%d)", desc->name,
                     -ELIB_DRV_ERR_ALREADY_EXIST);
        return -ELIB_DRV_ERR_ALREADY_EXIST;
    }
    slot = ctrl_slot_alloc(m);
    if (slot == NULL) {
        ELIB_DRV_LOG("[elib_drv] W: ctrl %s register failed (%d)", desc->name,
                     -ELIB_DRV_ERR_NO_SLOT);
        return -ELIB_DRV_ERR_NO_SLOT;
    }

    (void)memset(slot, 0, sizeof(*slot));
    slot->desc = *desc;
    slot->desc.ctx = &slot->ctx;
    slot->ctx.desc = &slot->desc;
    slot->ctx.lock = NULL;
    slot->ctx.device_count = 0;
    slot->ctx.open_count = 0;

    if (m->port != NULL) {
        slot->ctx.lock = m->port->mutex_create(slot->desc.name);
        if (slot->ctx.lock == NULL) {
            (void)memset(slot, 0, sizeof(*slot));
            ELIB_DRV_LOG("[elib_drv] W: ctrl %s register failed (%d)",
                         desc->name, -ELIB_DRV_ERR_LOCK);
            return -ELIB_DRV_ERR_LOCK;
        }
    }
    if (slot->desc.ops != NULL && slot->desc.ops->init != NULL) {
        rc = slot->desc.ops->init(&slot->ctx);
        if (rc < 0) {
            release_bound_devices(m, &slot->ctx);
            close_controller_direct(&slot->ctx);
            destroy_lock(m, &slot->ctx.lock);
            (void)memset(slot, 0, sizeof(*slot));
            ELIB_DRV_LOG("[elib_drv] E: ctrl %s init failed (%d)", desc->name,
                         rc);
            return rc;
        }
    }

    ELIB_DRV_LOG("[elib_drv] I: ctrl %s registered", desc->name);
    if (out != NULL) {
        *out = &slot->ctx;
    }
    return 0;
}

int elib_drv_register_device(const elib_drv_device_desc_t *desc,
                             elib_drv_device_t **out)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    elib_drv_device_slot_t *slot;
    elib_drv_controller_t *ctrl = NULL;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (desc == NULL || name_bad(desc->name)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (dev_find_any(m, desc->name) != NULL) {
        ELIB_DRV_LOG("[elib_drv] W: dev %s register failed (%d)", desc->name,
                     -ELIB_DRV_ERR_ALREADY_EXIST);
        return -ELIB_DRV_ERR_ALREADY_EXIST;
    }
    if (desc->controller != NULL) {
        ctrl = ctrl_find_any(m, desc->controller);
        if (ctrl == NULL) {
            ELIB_DRV_LOG("[elib_drv] W: dev %s register failed (%d)",
                         desc->name, -ELIB_DRV_ERR_NOT_FOUND);
            return -ELIB_DRV_ERR_NOT_FOUND;
        }
    }
    slot = dev_slot_alloc(m);
    if (slot == NULL) {
        ELIB_DRV_LOG("[elib_drv] W: dev %s register failed (%d)", desc->name,
                     -ELIB_DRV_ERR_NO_SLOT);
        return -ELIB_DRV_ERR_NO_SLOT;
    }

    (void)memset(slot, 0, sizeof(*slot));
    slot->desc = *desc;
    slot->desc.ctx = &slot->ctx;
    slot->ctx.desc = &slot->desc;
    slot->ctx.lock = NULL;
    slot->ctx.controller = ctrl;
    slot->ctx.open_count = 0;

    if (m->port != NULL) {
        slot->ctx.lock = m->port->mutex_create(slot->desc.name);
        if (slot->ctx.lock == NULL) {
            (void)memset(slot, 0, sizeof(*slot));
            ELIB_DRV_LOG("[elib_drv] W: dev %s register failed (%d)",
                         desc->name, -ELIB_DRV_ERR_LOCK);
            return -ELIB_DRV_ERR_LOCK;
        }
    }
    if (ctrl != NULL) {
        ctrl->device_count++;
    }

    ELIB_DRV_LOG("[elib_drv] I: dev %s registered", desc->name);
    if (out != NULL) {
        *out = &slot->ctx;
    }
    return 0;
}

static elib_drv_controller_slot_t *
ctrl_slot_owned(elib_drv_manager_t *m, const elib_drv_controller_t *ctx)
{
    unsigned i;

    for (i = 0; i < m->ctrl_capacity; i++) {
        if (&m->ctrl_slots[i].ctx == ctx) {
            return &m->ctrl_slots[i];
        }
    }
    return NULL;
}

static elib_drv_device_slot_t *
dev_slot_owned(elib_drv_manager_t *m, const elib_drv_device_t *ctx)
{
    unsigned i;

    for (i = 0; i < m->dev_capacity; i++) {
        if (&m->dev_slots[i].ctx == ctx) {
            return &m->dev_slots[i];
        }
    }
    return NULL;
}

int elib_drv_unregister_controller(elib_drv_controller_t *ctrl)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    elib_drv_controller_slot_t *slot;
    const char *name;
    int rc = 0;
    unsigned i;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    for (i = 0; i < m->controller_count; i++) {
        if (m->controllers[i].ctx == ctrl) {
            return -ELIB_DRV_ERR_NOT_SUPPORTED;
        }
    }
    slot = ctrl_slot_owned(m, ctrl);
    if (slot == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (ctrl->open_count != 0) {
        ELIB_DRV_LOG("[elib_drv] W: ctrl %s unregister failed (%d)",
                     slot->desc.name, -ELIB_DRV_ERR_BUSY);
        return -ELIB_DRV_ERR_BUSY;
    }
    if (ctrl->device_count != 0) {
        ELIB_DRV_LOG("[elib_drv] W: ctrl %s unregister failed (%d)",
                     slot->desc.name, -ELIB_DRV_ERR_BUSY);
        return -ELIB_DRV_ERR_BUSY;
    }

    name = slot->desc.name;
    (void)name; /* only read by the optional flow log */
    if (slot->desc.ops != NULL && slot->desc.ops->deinit != NULL) {
        rc = slot->desc.ops->deinit(&slot->ctx);
        if (rc < 0) {
            ELIB_DRV_LOG("[elib_drv] E: ctrl %s deinit failed (%d)", name, rc);
        }
    }
    destroy_lock(m, &slot->ctx.lock);
    (void)memset(slot, 0, sizeof(*slot));
    ELIB_DRV_LOG("[elib_drv] I: ctrl %s unregistered", name);
    return rc;
}

int elib_drv_unregister_device(elib_drv_device_t *dev)
{
    elib_drv_manager_t *m = &elib_drv_g_manager;
    elib_drv_device_slot_t *slot;
    const char *name;
    unsigned i;

    if (!m->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    for (i = 0; i < m->device_count; i++) {
        if (m->devices[i].ctx == dev) {
            return -ELIB_DRV_ERR_NOT_SUPPORTED;
        }
    }
    slot = dev_slot_owned(m, dev);
    if (slot == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (dev->open_count != 0) {
        ELIB_DRV_LOG("[elib_drv] W: dev %s unregister failed (%d)",
                     slot->desc.name, -ELIB_DRV_ERR_BUSY);
        return -ELIB_DRV_ERR_BUSY;
    }

    name = slot->desc.name;
    (void)name; /* only read by the optional flow log */
    if (dev->controller != NULL) {
        dev->controller->device_count--;
    }
    destroy_lock(m, &slot->ctx.lock);
    (void)memset(slot, 0, sizeof(*slot));
    ELIB_DRV_LOG("[elib_drv] I: dev %s unregistered", name);
    return 0;
}
