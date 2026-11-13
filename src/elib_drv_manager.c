/* elib_drv_manager.c - device tree validation, handle filling and lifecycle */
#include "elib_drv_internal.h"

#include <string.h>

elib_drv_manager_t elib_drv_g_manager;

static int port_valid(const elib_drv_port_t *port)
{
    if (port == NULL) {
        return 1;
    }
    return port->mutex_create != NULL && port->mutex_destroy != NULL &&
           port->mutex_lock != NULL && port->mutex_unlock != NULL;
}

int elib_drv_lock_helper(void *lock, uint32_t timeout_ms)
{
    if (lock == NULL || elib_drv_g_manager.port == NULL) {
        return 0;
    }
    return (elib_drv_g_manager.port->mutex_lock(lock, timeout_ms) == 0)
               ? 0
               : -ELIB_DRV_ERR_LOCK;
}

void elib_drv_unlock_helper(void *lock)
{
    if (lock != NULL && elib_drv_g_manager.port != NULL) {
        (void)elib_drv_g_manager.port->mutex_unlock(lock);
    }
}

static void fill_handles(const elib_drv_dt_node_t *dt_nodes, unsigned count,
                         elib_drv_node_t *out, unsigned *index,
                         elib_drv_node_t *parent)
{
    unsigned i;

    for (i = 0u; i < count; ++i) {
        elib_drv_node_t *handle = &out[*index];

        handle->dt = &dt_nodes[i];
        handle->controller = parent;
        handle->lock = NULL;
        handle->child_count = dt_nodes[i].child_count;
        handle->open_count = 0u;
        handle->init_order = 0u;
        handle->init_state = ELIB_DRV_NODE_INIT_NONE;
        ++(*index);

        if (dt_nodes[i].child_count > 0u) {
            fill_handles(dt_nodes[i].children, dt_nodes[i].child_count,
                         out, index, handle);
        }
    }
}

static int validate_names(const elib_drv_node_t *nodes, unsigned count)
{
    unsigned i, j;

    for (i = 0u; i < count; ++i) {
        const char *name = nodes[i].dt->name;

        if (name == NULL || name[0] == '\0') {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        for (j = 0u; j < i; ++j) {
            if (strcmp(nodes[j].dt->name, name) == 0) {
                return -ELIB_DRV_ERR_ALREADY_EXIST;
            }
        }
    }
    return 0;
}

static elib_drv_node_t *find_node_in_array(elib_drv_node_t *nodes,
                                           unsigned count, const char *name)
{
    unsigned i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0u; i < count; ++i) {
        if (nodes[i].dt->name != NULL &&
            strcmp(nodes[i].dt->name, name) == 0) {
            return &nodes[i];
        }
    }
    return NULL;
}

static int resolve_controllers(elib_drv_node_t *nodes, unsigned count)
{
    unsigned i;

    for (i = 0u; i < count; ++i) {
        const char *name = nodes[i].dt->controller;

        if (name == NULL) {
            continue;
        }
        if (name[0] == '\0') {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }

        {
            elib_drv_node_t *target =
                find_node_in_array(nodes, count, name);

            if (target == NULL) {
                return -ELIB_DRV_ERR_NOT_FOUND;
            }
            if (target == &nodes[i]) {
                return -ELIB_DRV_ERR_INVALID_PARAM;
            }
            nodes[i].controller = target;
        }
    }
    return 0;
}

static int init_node(elib_drv_node_t *node, unsigned depth)
{
    int rc;

    if (node == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (depth > ELIB_DRV_DT_MAX_DEPTH) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    if (node->init_state == ELIB_DRV_NODE_INIT_DONE) {
        return 0;
    }
    if (node->init_state == ELIB_DRV_NODE_INIT_IN_PROGRESS) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    node->init_state = ELIB_DRV_NODE_INIT_IN_PROGRESS;

    if (node->controller != NULL) {
        rc = init_node(node->controller, depth + 1u);
        if (rc != 0) {
            node->init_state = ELIB_DRV_NODE_INIT_NONE;
            return rc;
        }
    }

    if (node->dt->fops != NULL && node->dt->fops->init != NULL) {
        rc = node->dt->fops->init(node);
        if (rc != 0) {
            node->init_state = ELIB_DRV_NODE_INIT_NONE;
            return rc;
        }
    }

    node->init_order = ++elib_drv_g_manager.init_seq;
    node->init_state = ELIB_DRV_NODE_INIT_DONE;
    return 0;
}

static void destroy_locks(elib_drv_node_t *nodes, unsigned count)
{
    unsigned i;

    if (nodes == NULL || elib_drv_g_manager.port == NULL) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        if (nodes[i].lock != NULL) {
            elib_drv_g_manager.port->mutex_destroy(nodes[i].lock);
            nodes[i].lock = NULL;
        }
    }
}

static void reset_manager(void)
{
    elib_drv_g_manager.initialized = 0u;
    elib_drv_g_manager.initializing = 0u;
    elib_drv_g_manager.port = NULL;
    elib_drv_g_manager.tree = NULL;
    elib_drv_g_manager.nodes = NULL;
    elib_drv_g_manager.node_count = 0u;
    elib_drv_g_manager.init_seq = 0u;
    /* log and log_eol deliberately survive init/deinit. */
}

static int rollback_init(elib_drv_manager_t *mgr, int error)
{
    uint32_t order = mgr->init_seq;

    while (order > 0u) {
        unsigned i;

        for (i = 0u; i < mgr->node_count; ++i) {
            elib_drv_node_t *node = &mgr->nodes[i];

            if (node->init_order == order &&
                node->init_state == ELIB_DRV_NODE_INIT_DONE) {
                if (node->dt->fops != NULL &&
                    node->dt->fops->deinit != NULL) {
                    (void)node->dt->fops->deinit(node);
                }
                node->init_state = ELIB_DRV_NODE_INIT_NONE;
                node->init_order = 0u;
                break;
            }
        }
        --order;
    }

    destroy_locks(mgr->nodes, mgr->node_count);
    reset_manager();
    return error;
}

int elib_drv_init_tree(const elib_drv_port_t *port,
                       const elib_drv_dt_t *tree,
                       elib_drv_node_t *nodes,
                       unsigned node_capacity)
{
    unsigned node_count = 0u;
    unsigned i = 0u;
    int rc;

    if (elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_ALREADY_INIT;
    }
    if (!port_valid(port)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = elib_drv_dt_node_count(tree, &node_count);
    if (rc != 0) {
        return rc;
    }
    if (node_count > node_capacity ||
        (node_count > 0u && nodes == NULL)) {
        return -ELIB_DRV_ERR_NO_SLOT;
    }

    if (node_count == 0u) {
        elib_drv_g_manager.tree = tree;
        elib_drv_g_manager.nodes = nodes;
        elib_drv_g_manager.node_count = 0u;
        elib_drv_g_manager.init_seq = 0u;
        elib_drv_g_manager.port = port;
        elib_drv_g_manager.initialized = 1u;
        elib_drv_g_manager.initializing = 0u;
        ELIB_DRV_LOG_PLAIN("elib_drv: init tree (empty)");
        return 0;
    }

    fill_handles(tree->children, tree->child_count, nodes, &i, NULL);

    rc = validate_names(nodes, node_count);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_controllers(nodes, node_count);
    if (rc != 0) {
        return rc;
    }

    elib_drv_g_manager.port = port;
    for (i = 0u; i < node_count; ++i) {
        if (port != NULL) {
            nodes[i].lock = port->mutex_create(nodes[i].dt->name);
            if (nodes[i].lock == NULL) {
                destroy_locks(nodes, node_count);
                reset_manager();
                return -ELIB_DRV_ERR_LOCK;
            }
        }
    }

    elib_drv_g_manager.tree = tree;
    elib_drv_g_manager.nodes = nodes;
    elib_drv_g_manager.node_count = node_count;
    elib_drv_g_manager.init_seq = 0u;
    elib_drv_g_manager.initialized = 1u;
    elib_drv_g_manager.initializing = 1u;

    ELIB_DRV_LOG("elib_drv: init tree (%u nodes)", node_count);

    for (i = 0u; i < node_count; ++i) {
        rc = init_node(&nodes[i], 1u);
        if (rc != 0) {
            ELIB_DRV_LOG("elib_drv: init failed node=%s rc=%d",
                         nodes[i].dt->name, rc);
            return rollback_init(&elib_drv_g_manager, rc);
        }
    }

    elib_drv_g_manager.initializing = 0u;
    ELIB_DRV_LOG_PLAIN("elib_drv: init done");
    return 0;
}

int elib_drv_deinit(void)
{
    elib_drv_manager_t *mgr = &elib_drv_g_manager;
    unsigned i;
    uint32_t order;
    int first_error = 0;

    if (!mgr->initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (mgr->initializing) {
        return -ELIB_DRV_ERR_BUSY;
    }

    for (i = 0u; i < mgr->node_count; ++i) {
        if (mgr->nodes[i].open_count != 0u) {
            return -ELIB_DRV_ERR_BUSY;
        }
    }

    ELIB_DRV_LOG_PLAIN("elib_drv: deinit begin");

    order = mgr->init_seq;
    while (order > 0u) {
        for (i = 0u; i < mgr->node_count; ++i) {
            elib_drv_node_t *node = &mgr->nodes[i];

            if (node->init_order == order &&
                node->init_state == ELIB_DRV_NODE_INIT_DONE) {
                int rc = 0;

                if (node->dt->fops != NULL &&
                    node->dt->fops->deinit != NULL) {
                    rc = node->dt->fops->deinit(node);
                }
                if (rc != 0 && first_error == 0) {
                    first_error = rc;
                }
                node->init_state = ELIB_DRV_NODE_INIT_NONE;
                node->init_order = 0u;
                break;
            }
        }
        --order;
    }

    destroy_locks(mgr->nodes, mgr->node_count);
    reset_manager();
    ELIB_DRV_LOG_PLAIN("elib_drv: deinit done");
    return first_error;
}

elib_drv_node_t *elib_drv_node_find(const char *name)
{
    unsigned i;

    if (!elib_drv_g_manager.initialized || name == NULL) {
        return NULL;
    }
    for (i = 0u; i < elib_drv_g_manager.node_count; ++i) {
        elib_drv_node_t *node = &elib_drv_g_manager.nodes[i];

        if (node->dt->name != NULL && strcmp(node->dt->name, name) == 0) {
            return node;
        }
    }
    return NULL;
}

int elib_drv_node_foreach(int (*fn)(elib_drv_node_t *node, void *arg),
                          void *arg)
{
    unsigned i;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (fn == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    for (i = 0u; i < elib_drv_g_manager.node_count; ++i) {
        int rc = fn(&elib_drv_g_manager.nodes[i], arg);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int elib_drv_set_log(elib_drv_log_fn fn, unsigned newline)
{
    const char *eol;

    switch (newline) {
    case ELIB_DRV_LOG_NEWLINE_NONE:
        eol = "";
        break;
    case ELIB_DRV_LOG_NEWLINE_LF:
        eol = "\n";
        break;
    case ELIB_DRV_LOG_NEWLINE_CRLF:
        eol = "\r\n";
        break;
    default:
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    elib_drv_g_manager.log = fn;
    elib_drv_g_manager.log_eol = eol;
    return 0;
}
