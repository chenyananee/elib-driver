[简体中文](usage_device_io.md) | **English**

# Device Open and I/O

The device layer follows Linux `file_operations`: five entry points `open / close / read / write / ioctl`; the framework owns validation, reference counting, locking and logging, and the driver implements the five callbacks.

## Call sequence

```text
elib_drv_open(dev, flags)
  ├─ not initialized / bad arguments → return immediately
  ├─ take the device lock (timeout from desc.lock_timeout_ms)
  ├─ EXCLUSIVE with an existing opener → -BUSY
  ├─ on 0 → 1 call ops->open(dev, flags)
  ├─ open_count++, release the lock
  └─ log open outside the lock

read / write / ioctl
  ├─ take the device lock
  ├─ open_count == 0 → -NOT_OPEN
  ├─ ops or the matching hook is NULL → -NOT_SUPPORTED
  ├─ call ops->read/write/ioctl under the lock
  └─ release; a read/write result greater than len maps to -IO
```

## Open and close

- Open is **reference counted**: three opens call `ops->open` once; `open_count` becomes 1/2/3.
- `ELIB_DRV_DEV_F_EXCLUSIVE`: a second open while one is active returns `-ELIB_DRV_ERR_BUSY`, even from the same caller.
- `close` decrements the count first and calls `ops->close` only at zero; with a zero count it returns `-ELIB_DRV_ERR_NOT_OPEN`.
- Close failure semantics: the count is already released, the framework returns the `ops->close` error and logs it, and the device is treated as closed.

| API | Behaviour | Failure sources |
|---|---|---|
| `elib_drv_open` | increment; call `ops->open` on 0→1 | `INVALID_PARAM`, `BUSY`, `LOCK`, driver error |
| `elib_drv_close` | decrement; call `ops->close` at zero | `NOT_OPEN`, `LOCK`, driver error |
| `elib_drv_read` | call `ops->read` under the device lock | `INVALID_PARAM`, `NOT_OPEN`, `NOT_SUPPORTED`, `LOCK`, `IO` |
| `elib_drv_write` | call `ops->write` under the device lock | same as read |
| `elib_drv_ioctl` | call `ops->ioctl` under the device lock | `INVALID_PARAM`, `NOT_OPEN`, `NOT_SUPPORTED`, `LOCK` |

## I/O rules

- `read`/`write` may return 0 or fewer than `len` bytes, meaning a short read/write; a value greater than `len` is mapped to `-ELIB_DRV_ERR_IO`.
- `ioctl` commands are built with `ELIB_DRV_IOC(group, nr)`: `group` and `nr` are 8 bits each, and a positive return value is driver defined.
- `ops` is called **under the device lock**: the implementation must not call `elib_drv_open/close/read/write/ioctl` on the same device again (a non-recursive mutex self-deadlocks); to touch a shared bus, take the controller lock as described in [Direct controller access](usage_controller_direct.en.md).
- The device lock timeout is `device_desc.lock_timeout_ms`; `0` waits forever and a failure returns `-ELIB_DRV_ERR_LOCK`.

## Argument validation

| Input | Checked in | Return |
|---|---|---|
| `dev == NULL` or `dev->desc == NULL` | every device API | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `len > INT_MAX` | `read`/`write` | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `buf == NULL && len > 0` | `read`/`write` | `-ELIB_DRV_ERR_INVALID_PARAM` |
| not open | `read`/`write`/`ioctl` | `-ELIB_DRV_ERR_NOT_OPEN` |
| `ops` or the matching hook is `NULL` | `read`/`write`/`ioctl` | `-ELIB_DRV_ERR_NOT_SUPPORTED` |
| the driver returns `> len` | `read`/`write` | `-ELIB_DRV_ERR_IO` |

## Example

```c
/* driver side: five callbacks, no locking needed */
static int eeprom_open(elib_drv_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    return 0;
}

static int eeprom_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    return eeprom_burst_read(elib_drv_device_priv(dev), buf, len);
}

static const elib_drv_ops_t eeprom_ops = {
    eeprom_open, eeprom_close, eeprom_read, eeprom_write, eeprom_ioctl
};

/* user side */
elib_drv_device_t *dev = elib_drv_device_find("eeprom");
uint8_t buf[16];

if (elib_drv_open(dev, 0) == 0) {
    int n = elib_drv_read(dev, buf, sizeof buf);   /* 0..16 */
    if (n < 0) {
        /* -NOT_OPEN / -NOT_SUPPORTED / -IO / -LOCK */
    }
    (void)elib_drv_close(dev);
}
```

## Pitfalls

- Calling `elib_drv_read(dev, ...)` from inside `ops->read` self-deadlocks; extract the shared logic into an internal function that bypasses the framework entry.
- A failing `close` does not mean the count was kept; never close again just because close failed.
- `EXCLUSIVE` limits concurrent openers, not repeated opens by the same caller: the second open already returns BUSY.
- I/O results are only checked for an upper bound; negative driver errors pass through unchanged.

## See also

- [Direct controller access](usage_controller_direct.en.md)
- [Controller lock and concurrency](usage_locking.en.md)
- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Back to README](../README.en.md)
