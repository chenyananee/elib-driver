/* elib_drv_device.c - device side: file_operations dispatch, per device
 * locking, accessors and the shared controller lock. */
#include "elib_drv_internal.h"

#include <limits.h>

static uint32_t device_timeout(const elib_drv_device_t *dev)
{
    uint32_t timeout = dev->desc->lock_timeout_ms;

    return (timeout == 0) ? ELIB_DRV_WAIT_FOREVER : timeout;
}

static int lock_device(elib_drv_device_t *dev)
{
    int rc = elib_drv_lock_helper(dev->lock, device_timeout(dev));

    if (rc != 0) {
        ELIB_DRV_LOG("[elib_drv] E: %s lock timeout", dev->desc->name);
    }
    return rc;
}

int elib_drv_open(elib_drv_device_t *dev, uint32_t flags)
{
    const elib_drv_ops_t *ops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL || dev->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_device(dev);
    if (rc != 0) {
        return rc;
    }
    if (((dev->desc->flags & ELIB_DRV_DEV_F_EXCLUSIVE) != 0) &&
        dev->open_count != 0) {
        elib_drv_unlock_helper(dev->lock);
        ELIB_DRV_LOG("[elib_drv] W: %s open failed (%d)", dev->desc->name,
                     -ELIB_DRV_ERR_BUSY);
        return -ELIB_DRV_ERR_BUSY;
    }
    ops = dev->desc->ops;
    if (dev->open_count == 0) {
        rc = (ops != NULL && ops->open != NULL) ? ops->open(dev, flags) : 0;
        if (rc < 0) {
            elib_drv_unlock_helper(dev->lock);
            ELIB_DRV_LOG("[elib_drv] E: %s open failed (%d)", dev->desc->name,
                         rc);
            return rc;
        }
    }
    dev->open_count++;
    elib_drv_unlock_helper(dev->lock);
    ELIB_DRV_LOG("[elib_drv] I: %s open", dev->desc->name);
    return 0;
}

int elib_drv_close(elib_drv_device_t *dev)
{
    const elib_drv_ops_t *ops;
    int rc = 0;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL || dev->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_device(dev);
    if (rc != 0) {
        return rc;
    }
    if (dev->open_count == 0) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    dev->open_count--;
    ops = dev->desc->ops;
    rc = 0;
    if (dev->open_count == 0 && ops != NULL && ops->close != NULL) {
        rc = ops->close(dev);
    }
    elib_drv_unlock_helper(dev->lock);
    if (rc < 0) {
        ELIB_DRV_LOG("[elib_drv] E: %s close failed (%d)", dev->desc->name,
                     rc);
    } else {
        ELIB_DRV_LOG("[elib_drv] I: %s close", dev->desc->name);
    }
    return rc;
}

int elib_drv_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    const elib_drv_ops_t *ops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL || dev->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (buf == NULL && len > 0) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_device(dev);
    if (rc != 0) {
        return rc;
    }
    if (dev->open_count == 0) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    ops = dev->desc->ops;
    if (ops == NULL || ops->read == NULL) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = ops->read(dev, buf, len);
    elib_drv_unlock_helper(dev->lock);
    if (rc > (int)len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_write(elib_drv_device_t *dev, const void *buf, size_t len)
{
    const elib_drv_ops_t *ops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL || dev->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (buf == NULL && len > 0) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_device(dev);
    if (rc != 0) {
        return rc;
    }
    if (dev->open_count == 0) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    ops = dev->desc->ops;
    if (ops == NULL || ops->write == NULL) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = ops->write(dev, buf, len);
    elib_drv_unlock_helper(dev->lock);
    if (rc > (int)len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_ioctl(elib_drv_device_t *dev, uint32_t cmd, void *arg)
{
    const elib_drv_ops_t *ops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (dev == NULL || dev->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_device(dev);
    if (rc != 0) {
        return rc;
    }
    if (dev->open_count == 0) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    ops = dev->desc->ops;
    if (ops == NULL || ops->ioctl == NULL) {
        elib_drv_unlock_helper(dev->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = ops->ioctl(dev, cmd, arg);
    elib_drv_unlock_helper(dev->lock);
    return rc;
}

/* ---- controller direct access ------------------------------------------ */

static uint32_t controller_timeout(const elib_drv_controller_t *ctrl)
{
    uint32_t timeout = ctrl->desc->lock_timeout_ms;

    return (timeout == 0) ? ELIB_DRV_WAIT_FOREVER : timeout;
}

static int lock_controller(elib_drv_controller_t *ctrl)
{
    int rc = elib_drv_lock_helper(ctrl->lock, controller_timeout(ctrl));

    if (rc != 0) {
        ELIB_DRV_LOG("[elib_drv] E: ctrl %s lock timeout", ctrl->desc->name);
    }
    return rc;
}

int elib_drv_controller_open(elib_drv_controller_t *ctrl, uint32_t flags)
{
    const elib_drv_controller_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_controller(ctrl);
    if (rc != 0) {
        return rc;
    }
    fops = ctrl->desc->fops;
    if (ctrl->open_count == 0) {
        rc = (fops != NULL && fops->open != NULL) ? fops->open(ctrl, flags) : 0;
        if (rc < 0) {
            elib_drv_unlock_helper(ctrl->lock);
            ELIB_DRV_LOG("[elib_drv] E: ctrl %s open failed (%d)",
                         ctrl->desc->name, rc);
            return rc;
        }
    }
    ctrl->open_count++;
    elib_drv_unlock_helper(ctrl->lock);
    ELIB_DRV_LOG("[elib_drv] I: ctrl %s open", ctrl->desc->name);
    return 0;
}

int elib_drv_controller_close(elib_drv_controller_t *ctrl)
{
    const elib_drv_controller_fops_t *fops;
    int rc = 0;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_controller(ctrl);
    if (rc != 0) {
        return rc;
    }
    if (ctrl->open_count == 0) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    ctrl->open_count--;
    fops = ctrl->desc->fops;
    rc = 0;
    if (ctrl->open_count == 0 && fops != NULL && fops->close != NULL) {
        rc = fops->close(ctrl);
    }
    elib_drv_unlock_helper(ctrl->lock);
    if (rc < 0) {
        ELIB_DRV_LOG("[elib_drv] E: ctrl %s close failed (%d)",
                     ctrl->desc->name, rc);
    } else {
        ELIB_DRV_LOG("[elib_drv] I: ctrl %s close", ctrl->desc->name);
    }
    return rc;
}

int elib_drv_controller_read(elib_drv_controller_t *ctrl, void *buf, size_t len)
{
    const elib_drv_controller_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (buf == NULL && len > 0) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_controller(ctrl);
    if (rc != 0) {
        return rc;
    }
    if (ctrl->open_count == 0) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    fops = ctrl->desc->fops;
    if (fops == NULL || fops->read == NULL) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = fops->read(ctrl, buf, len);
    elib_drv_unlock_helper(ctrl->lock);
    if (rc > (int)len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_controller_write(elib_drv_controller_t *ctrl, const void *buf,
                              size_t len)
{
    const elib_drv_controller_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (len > (size_t)INT_MAX) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    if (buf == NULL && len > 0) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_controller(ctrl);
    if (rc != 0) {
        return rc;
    }
    if (ctrl->open_count == 0) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    fops = ctrl->desc->fops;
    if (fops == NULL || fops->write == NULL) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = fops->write(ctrl, buf, len);
    elib_drv_unlock_helper(ctrl->lock);
    if (rc > (int)len) {
        return -ELIB_DRV_ERR_IO;
    }
    return rc;
}

int elib_drv_controller_ioctl(elib_drv_controller_t *ctrl, uint32_t cmd,
                              void *arg)
{
    const elib_drv_controller_fops_t *fops;
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = lock_controller(ctrl);
    if (rc != 0) {
        return rc;
    }
    if (ctrl->open_count == 0) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_OPEN;
    }
    fops = ctrl->desc->fops;
    if (fops == NULL || fops->ioctl == NULL) {
        elib_drv_unlock_helper(ctrl->lock);
        return -ELIB_DRV_ERR_NOT_SUPPORTED;
    }
    rc = fops->ioctl(ctrl, cmd, arg);
    elib_drv_unlock_helper(ctrl->lock);
    return rc;
}

/* ---- accessors --------------------------------------------------------- */

const char *elib_drv_controller_name(const elib_drv_controller_t *ctrl)
{
    return (ctrl != NULL && ctrl->desc != NULL) ? ctrl->desc->name : NULL;
}

void *elib_drv_controller_priv(const elib_drv_controller_t *ctrl)
{
    return (ctrl != NULL && ctrl->desc != NULL) ? ctrl->desc->priv : NULL;
}

unsigned elib_drv_controller_open_count(const elib_drv_controller_t *ctrl)
{
    return (ctrl != NULL) ? (unsigned)ctrl->open_count : 0u;
}

unsigned elib_drv_controller_device_count(const elib_drv_controller_t *ctrl)
{
    return (ctrl != NULL) ? (unsigned)ctrl->device_count : 0u;
}

const char *elib_drv_device_name(const elib_drv_device_t *dev)
{
    return (dev != NULL && dev->desc != NULL) ? dev->desc->name : NULL;
}

void *elib_drv_device_priv(const elib_drv_device_t *dev)
{
    return (dev != NULL && dev->desc != NULL) ? dev->desc->priv : NULL;
}

elib_drv_controller_t *elib_drv_device_controller(const elib_drv_device_t *dev)
{
    return (dev != NULL) ? dev->controller : NULL;
}

unsigned elib_drv_device_open_count(const elib_drv_device_t *dev)
{
    return (dev != NULL) ? (unsigned)dev->open_count : 0u;
}

/* ---- controller lock --------------------------------------------------- */

int elib_drv_controller_lock(elib_drv_controller_t *ctrl, uint32_t timeout_ms)
{
    int rc;

    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    rc = elib_drv_lock_helper(ctrl->lock, timeout_ms);
    if (rc != 0) {
        ELIB_DRV_LOG("[elib_drv] E: ctrl %s lock timeout", ctrl->desc->name);
    }
    return rc;
}

int elib_drv_controller_unlock(elib_drv_controller_t *ctrl)
{
    if (!elib_drv_g_manager.initialized) {
        return -ELIB_DRV_ERR_NOT_INITIALIZED;
    }
    if (ctrl == NULL || ctrl->desc == NULL) {
        return -ELIB_DRV_ERR_INVALID_PARAM;
    }
    elib_drv_unlock_helper(ctrl->lock);
    return 0;
}
