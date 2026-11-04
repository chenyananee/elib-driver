[简体中文](usage_locking.md) | **English**

# Locking, Concurrency and Timeouts

The framework only uses the mutexes provided by the port: the device path takes the device lock, the direct path takes the controller lock, and a driver takes the controller lock inside device ops when needed. With `port == NULL` no lock is created or taken at all.

## Port contract

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

- All four callbacks are required, otherwise `elib_drv_init` returns `-ELIB_DRV_ERR_INVALID_PARAM`.
- `mutex_create` returning `NULL` is a lock creation failure: `init` rolls back with `-ELIB_DRV_ERR_LOCK`.
- `mutex_lock` returns 0 on success and a negative value on timeout/failure; the framework maps it to `-ELIB_DRV_ERR_LOCK`.
- The port owns the mutex storage (heap or a static pool); the framework never allocates.

## What each lock covers

| Lock | Protects | Taken by |
|---|---|---|
| device lock | open count, device state, `ops` serialisation | framework (device API) |
| controller lock | bus/shared registers, direct `fops` serialisation | framework (direct API); the driver inside device ops |

- The device lock is held only during `elib_drv_open/close/read/write/ioctl`.
- The controller lock is held by the framework during direct `open/close/read/write/ioctl`; a device driver takes it explicitly with `elib_drv_controller_lock` when it touches a shared bus.
- The log callback runs outside the locks and must not call any `elib_drv_*` function.

## Timeouts

| Setting | Behaviour |
|---|---|
| `lock_timeout_ms == 0` | mapped to `ELIB_DRV_WAIT_FOREVER` (`UINT32_MAX`), wait forever |
| `lock_timeout_ms > 0` | wait that many milliseconds; timeout returns `-ELIB_DRV_ERR_LOCK` |
| direct path | uses `controller_desc.lock_timeout_ms` |
| device path | uses `device_desc.lock_timeout_ms`; a controller lock taken by the driver uses the driver's own timeout |

## Lock order

- `device lock → controller lock` is allowed; the **reverse is forbidden**.
- A direct call holds only the controller lock and never nests a device lock.
- Inside the `ops`/`fops` of the same device/controller, never call the matching `elib_drv_*open/close/read/write/ioctl` again (a non-recursive mutex self-deadlocks).

## Concurrency matrix

| API group | Concurrent with I/O | Concurrent with register/unregister | Notes |
|---|---|---|---|
| `elib_drv_init` / `elib_drv_deinit` | no | no | startup/shutdown only |
| `elib_drv_register_*` / `unregister_*` | no | no | device discovery time |
| `elib_drv_controller_find` / `elib_drv_device_find` / `elib_drv_device_foreach` | yes | no | read only; unregister leaves dangling pointers |
| `elib_drv_open` / `close` / `read` / `write` / `ioctl` | yes | no | serialised by the device lock |
| direct controller APIs | yes | no | serialised by the controller lock |
| `elib_drv_controller_lock` / `unlock` | yes | no | for drivers; mind the order |

## find / foreach

- `find` / `foreach` take no lock and return internal ctx pointers; they may run concurrently with I/O.
- They must **not** run concurrently with `register_*` / `unregister_*`: a pointer becomes invalid immediately after unregistration.
- A `foreach` callback must not register/unregister devices or tear down the manager; a non-zero return stops the walk and is returned unchanged.
- The walk order is fixed: static table first, then runtime slots in index order.

## ISR and reentrancy

- All APIs are task-context only; an ISR must use its own mechanism (flag/queue plus the main loop).
- A driver `ops`/`fops` must not re-enter the framework entry of the same device/controller.
- The log callback may run concurrently from several tasks: it must be thread safe and must not call `elib_drv_*`.

## Example

```c
/* a driver sharing a bus inside device ops: device -> controller order */
static int sensor_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    elib_drv_controller_t *bus = elib_drv_device_controller(dev);
    int rc = elib_drv_controller_lock(bus, ELIB_DRV_WAIT_FOREVER);

    if (rc != 0) {
        return rc;                       /* -ELIB_DRV_ERR_LOCK */
    }
    rc = i2c_burst_read(elib_drv_controller_priv(bus), dev, buf, len);
    (void)elib_drv_controller_unlock(bus);
    return rc;
}
```

## See also

- [Direct controller access](usage_controller_direct.en.md)
- [Device open and I/O](usage_device_io.en.md)
- [RTOS porting](usage_porting.en.md)
- [Back to README](../README.en.md)
