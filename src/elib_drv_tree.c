/* elib_drv_tree.c - device tree counting, lookup and of style queries */
#include "elib_drv_internal.h"

#include <string.h>

static int count_children(const elib_drv_dt_node_t *nodes, unsigned count,
                          unsigned depth, unsigned *total)
{
    unsigned i;

    if (count > 0u && nodes == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (depth > ELIB_DRV_DT_MAX_DEPTH) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    for (i = 0u; i < count; ++i) {
        unsigned child_total = 0u;

        if (*total == UINT32_MAX) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        ++(*total);

        if (nodes[i].child_count > 0u && nodes[i].children == NULL) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        if (count_children(nodes[i].children, nodes[i].child_count,
                           depth + 1u, &child_total) != 0) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        if (*total > UINT32_MAX - child_total) {
            return -ELIB_DRV_ERR_INVALID_PARAM;
        }
        *total += child_total;
    }

    return 0;
}

int elib_drv_tree_count(const elib_drv_dt_t *tree, unsigned *out_count)
{
    unsigned total = 0u;
    int rc;

    if (tree == NULL || out_count == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (tree->child_count > 0u && tree->children == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = count_children(tree->children, tree->child_count, 1u, &total);
    if (rc != 0) {
        return rc;
    }
    *out_count = total;
    return 0;
}

int elib_drv_dt_node_count(const elib_drv_dt_t *tree, unsigned *out_count)
{
    return elib_drv_tree_count(tree, out_count);
}

static const elib_drv_dt_node_t *find_dt_children(
    const elib_drv_dt_node_t *nodes, unsigned count, const char *name)
{
    unsigned i;

    if (nodes == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0u; i < count; ++i) {
        const elib_drv_dt_node_t *found;

        if (nodes[i].name != NULL && strcmp(nodes[i].name, name) == 0) {
            return &nodes[i];
        }
        found = find_dt_children(nodes[i].children, nodes[i].child_count,
                                 name);
        if (found != NULL) {
            return found;
        }
    }

    return NULL;
}

const elib_drv_dt_node_t *elib_drv_tree_find_dt(const elib_drv_dt_t *tree,
                                                const char *name)
{
    if (tree == NULL || name == NULL) {
        return NULL;
    }
    return find_dt_children(tree->children, tree->child_count, name);
}

elib_drv_node_t *elib_drv_node_from_dt(const elib_drv_dt_node_t *dt)
{
    unsigned i;

    if (!elib_drv_g_manager.initialized || dt == NULL) {
        return NULL;
    }
    for (i = 0u; i < elib_drv_g_manager.node_count; ++i) {
        if (elib_drv_g_manager.nodes[i].dt == dt) {
            return &elib_drv_g_manager.nodes[i];
        }
    }
    return NULL;
}

const elib_drv_dt_node_t *elib_drv_of_find(const char *name)
{
    if (!elib_drv_g_manager.initialized || name == NULL) {
        return NULL;
    }
    return elib_drv_tree_find_dt(elib_drv_g_manager.tree, name);
}

const void *elib_drv_of_config(const elib_drv_dt_node_t *dt)
{
    return (dt != NULL) ? dt->ctx : NULL;
}

const char *elib_drv_of_name(const elib_drv_dt_node_t *dt)
{
    return (dt != NULL) ? dt->name : NULL;
}

const char *elib_drv_of_controller(const elib_drv_dt_node_t *dt)
{
    return (dt != NULL) ? dt->controller : NULL;
}

const elib_drv_dt_node_t *elib_drv_of_children(
    const elib_drv_dt_node_t *dt, unsigned *child_count)
{
    if (child_count != NULL) {
        *child_count = (dt != NULL) ? dt->child_count : 0u;
    }
    return (dt != NULL) ? dt->children : NULL;
}
