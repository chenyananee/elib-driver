/* elib_drv_node.c - unified node I/O, node locks and accessors */
#include "elib_drv_internal.h"

#include <limits.h>

static int node_valid(const elib_drv_node_t *node)
{
    return node != NULL && node->dt != NULL;
}

static uint32_t node_timeout(const elib_drv_node_t *node)
{
    uint32_t timeout = node->dt->lock_timeout_ms;

    return (timeout == 0u) ? ELIB_DRV_WAIT_FOREVER : timeout;
}

static int lock_node(elib_drv_node_t *node)
{
    int rc = elib_drv_lock_helper(node->lock, node_timeout(node));

    if (rc != 0) {
        ELIB_DRV_LOG("elib_drv: E: %s lock timeout", node->dt->name);
    }
    return rc;
}

int elib_drv_open(elib_drv_node_t *node, uint32_t flags)
{
    const elib_drv_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = lock_node(node);
    if (rc != 0) {
        return rc;
    }

    if (((node->dt->flags & ELIB_DRV_DEV_F_EXCLUSIVE) != 0u) &&
        node->open_count != 0u) {
        elib_drv_unlock_helper(node->lock);
        ELIB_DRV_LOG("elib_drv: W: %s open busy", node->dt->name);
        return -ELIB_DRV_ERR_BUSY;
    }

    fops = node->dt->fops;
    if (node->open_count == 0u) {
        rc = (fops != NULL && fops->open != NULL) ? fops->open(node, flags) : 0;
        if (rc < 0) {
            elib_drv_unlock_helper(node->lock);
            ELIB_DRV_LOG("elib_drv: E: %s open failed (%d)",
                         node->dt->name, rc);
            return rc;
        }
    }

    ++node->open_count;
    elib_drv_unlock_helper(node->lock);
    ELIB_DRV_LOG("elib_drv: I: %s open", node->dt->name);
    return 0;
}

int elib_drv_close(elib_drv_node_t *node)
{
    const elib_drv_fops_t *fops;
    int rc = 0;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = lock_node(node);
    if (rc != 0) {
        return rc;
    }
    if (node->open_count == 0u) {
        elib_drv_unlock_helper(node->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }

    --node->open_count;
    fops = node->dt->fops;
    if (node->open_count == 0u && fops != NULL && fops->close != NULL) {
        rc = fops->close(node);
    }
    elib_drv_unlock_helper(node->lock);

    if (rc < 0) {
        ELIB_DRV_LOG("elib_drv: E: %s close failed (%d)", node->dt->name, rc);
    } else {
        ELIB_DRV_LOG("elib_drv: I: %s close", node->dt->name);
    }
    return rc;
}

int elib_drv_read(elib_drv_node_t *node, void *buf, size_t len)
{
    const elib_drv_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX || (buf == NULL && len != 0u)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = lock_node(node);
    if (rc != 0) {
        return rc;
    }

    fops = node->dt->fops;
    if (fops == NULL || fops->read == NULL) {
        elib_drv_unlock_helper(node->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }

    rc = fops->read(node, buf, len);
    elib_drv_unlock_helper(node->lock);

    if (rc < 0) {
        return rc;
    }
    if ((size_t)rc > len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_write(elib_drv_node_t *node, const void *buf, size_t len)
{
    const elib_drv_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX || (buf == NULL && len != 0u)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = lock_node(node);
    if (rc != 0) {
        return rc;
    }

    fops = node->dt->fops;
    if (fops == NULL || fops->write == NULL) {
        elib_drv_unlock_helper(node->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }

    rc = fops->write(node, buf, len);
    elib_drv_unlock_helper(node->lock);

    if (rc < 0) {
        return rc;
    }
    if ((size_t)rc > len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg)
{
    const elib_drv_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }

    rc = lock_node(node);
    if (rc != 0) {
        return rc;
    }

    fops = node->dt->fops;
    if (fops == NULL || fops->ioctl == NULL) {
        elib_drv_unlock_helper(node->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }

    rc = fops->ioctl(node, cmd, arg);
    elib_drv_unlock_helper(node->lock);
    return rc;
}

int elib_drv_node_lock(elib_drv_node_t *node, uint32_t timeout_ms)
{
    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (timeout_ms == 0u) {
        timeout_ms = ELIB_DRV_WAIT_FOREVER;
    }
    return elib_drv_lock_helper(node->lock, timeout_ms);
}

int elib_drv_node_unlock(elib_drv_node_t *node)
{
    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (!node_valid(node)) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    elib_drv_unlock_helper(node->lock);
    return 0;
}

const char *elib_drv_node_name(const elib_drv_node_t *node)
{
    return node_valid(node) ? node->dt->name : NULL;
}

void *elib_drv_node_ctx(const elib_drv_node_t *node)
{
    return node_valid(node) ? node->dt->ctx : NULL;
}

elib_drv_node_t *elib_drv_node_controller(const elib_drv_node_t *node)
{
    return node_valid(node) ? node->controller : NULL;
}

const char *elib_drv_node_controller_name(const elib_drv_node_t *node)
{
    if (!node_valid(node) || node->controller == NULL) {
        return NULL;
    }
    return node->controller->dt->name;
}

unsigned elib_drv_node_open_count(const elib_drv_node_t *node)
{
    return node_valid(node) ? (unsigned)node->open_count : 0u;
}

unsigned elib_drv_node_child_count(const elib_drv_node_t *node)
{
    return node_valid(node) ? (unsigned)node->child_count : 0u;
}
