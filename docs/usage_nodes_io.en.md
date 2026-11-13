[简体中文](usage_nodes_io.md) | **English**

# Unified Node I/O

[Back to README](../README.en.md)

## Unified fops

Controllers and devices use the same table:

```c
typedef struct {
    int (*init)(elib_drv_node_t *node);
    int (*deinit)(elib_drv_node_t *node);
    int (*open)(elib_drv_node_t *node, uint32_t flags);
    int (*close)(elib_drv_node_t *node);
    int (*read)(elib_drv_node_t *node, void *buf, size_t len);
    int (*write)(elib_drv_node_t *node, const void *buf, size_t len);
    int (*ioctl)(elib_drv_node_t *node, uint32_t cmd, void *arg);
} elib_drv_fops_t;
```

Every hook is optional. Driver private data lives in the node `ctx` and is
retrieved with `elib_drv_node_ctx(node)`.

## Automatic Init

`elib_drv_init_tree()` calls each node's `fops->init` once, if present:

- A controller is initialized before its dependent nodes; a shared controller
  is initialized once.
- One node gets exactly one init call per init run.
- A deinit/re-init cycle calls init again, so fops must be idempotent.
- On failure the original error is returned and successfully initialized nodes
  are deinitialized in reverse order.

## open / close

- `open/close` are **optional**; they only call fops->open/close, maintain the
  reference count and handle `ELIB_DRV_DEV_F_EXCLUSIVE`.
- The first open calls fops->open; the last close calls fops->close.
- read/write/ioctl work without a prior open, suitable for drivers whose init
  already brings the hardware up.
- Opening an already-open exclusive node returns `-BUSY`.

## read / write / ioctl

```c
int elib_drv_read(elib_drv_node_t *node, void *buf, size_t len);
int elib_drv_write(elib_drv_node_t *node, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg);
```

- Work on any node; the framework dispatches through `node->dt->fops`.
- read/write call fops under the node lock; a result greater than len maps to
  `-ELIB_DRV_ERR_IO`.
- Missing fops return `-ELIB_DRV_ERR_NOT_SUPPORTED`.
- `buf == NULL && len != 0` returns `-INVALID_PARAM`.

## Device Forwarding to a Controller

A device driver looks up its configured controller by name and calls the same
I/O API:

```c
static int sensor_read(elib_drv_node_t *node, void *buf, size_t len)
{
    sensor_cfg_t *cfg = (sensor_cfg_t *)elib_drv_node_ctx(node);
    elib_drv_node_t *ctrl =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }

    /* Target/register selection is a controller convention; it can be done
     * first with elib_drv_ioctl(ctrl, ..., cfg). */
    return elib_drv_read(ctrl, buf, len);
}
```

- The framework takes the controller node lock around the nested
  `elib_drv_read(ctrl, ...)`.
- Ordering is `device lock -> controller lock`; the reverse is forbidden.
- A device must not call SPI/I2C low-level functions directly; controller type
  details belong in controller fops.
- If multiple devices on one controller need an atomic "select target +
  transfer", the controller should expose a combined ioctl or make selection
  and transfer indivisible inside its fops; the framework only guarantees a
  single call holds the lock.

## Direct Controller Access

A UART, bare ADC, GPIO and similar nodes are driven directly:

```c
elib_drv_node_t *uart = elib_drv_node_find("uart0");

(void)elib_drv_open(uart, 0u);
(void)elib_drv_write(uart, "AT\r\n", 4u);
(void)elib_drv_close(uart);
```

This uses the same API and the same controller lock as device forwarding.

## Explicit Locking

A driver that needs to group several controller operations can lock explicitly:

```c
if (elib_drv_node_lock(ctrl, 100u) != 0) {
    return -ELIB_DRV_ERR_LOCK;
}
/* call low-level/controller operations here; do not call elib_drv_read(ctrl)
 * or another API that would lock the same node again */
(void)elib_drv_node_unlock(ctrl);
```

`elib_drv_read/write/ioctl` lock the node themselves; do not call them while
already holding the same node lock.

## Errors and Return Values

| Case | Return |
|------|--------|
| manager not initialized | `-NOT_INITIALIZED` |
| invalid node/fops argument | `-INVALID_PARAM` |
| missing fops hook | `-NOT_SUPPORTED` |
| lock timeout/failure | `-LOCK` |
| close of an unopened node | `-NOT_OPEN` |
| read/write driver result > len | `-IO` |
| node/controller lookup failure | `-NOT_FOUND` / `-INVALID_PARAM` at init |
