[简体中文](usage_controller_direct.md) | **English**

# Direct Controller Access

Not every peripheral needs a device layer. A controller such as a UART, a bare ADC or a GPIO bank can expose only `fops`, and the application opens/reads/writes it directly; the framework still owns the lock, reference counting and logging.

## When to use

- The controller itself is the final object (serial port, ADC channel, GPIO group);
- You do not want to wrap every instance in another device;
- You need to share the same bus lock with devices bound to that controller.

## The fops table

```c
typedef struct {
    int (*open)(elib_drv_controller_t *ctrl, uint32_t flags);
    int (*close)(elib_drv_controller_t *ctrl);
    int (*read)(elib_drv_controller_t *ctrl, void *buf, size_t len);
    int (*write)(elib_drv_controller_t *ctrl, const void *buf, size_t len);
    int (*ioctl)(elib_drv_controller_t *ctrl, uint32_t cmd, void *arg);
} elib_drv_controller_fops_t;
```

- All five hooks may be `NULL`; the whole `fops` table may be `NULL` too.
- With `fops == NULL` or a `NULL` hook, open/close still work (counting only) and the matching I/O returns `-ELIB_DRV_ERR_NOT_SUPPORTED`.
- `priv` is retrieved with `elib_drv_controller_priv(ctrl)`; the framework never touches it.

## Calls and locking

```text
elib_drv_controller_open/close/read/write/ioctl(ctrl)
  ├─ not initialized / bad arguments → return immediately
  ├─ take the controller lock (timeout from controller_desc.lock_timeout_ms)
  ├─ I/O with no direct open → -NOT_OPEN
  ├─ call the matching fops hook under the lock
  ├─ release the lock
  └─ log open/close outside the lock
```

Direct open/close counts are **independent** of device open counts: an open device does not affect direct open and vice versa; but both use the **same controller lock** when touching the hardware.

## Rules

- While a direct open is active, `elib_drv_deinit` and `elib_drv_unregister_controller` return `-ELIB_DRV_ERR_BUSY`.
- `fops` is called under the controller lock: the implementation must **not** call `elib_drv_controller_lock` again, nor call back into `elib_drv_device_*` (that would invert the device → controller lock order).
- Direct calls hold only the controller lock and never nest a device lock.
- Direct I/O validates arguments like the device path: `len > INT_MAX` and `buf == NULL && len > 0` return `INVALID_PARAM`; a result greater than `len` maps to `-IO`.

| Aspect | Device path | Direct controller path |
|---|---|---|
| Handle | `elib_drv_device_t *` | `elib_drv_controller_t *` |
| Open count | per device | per controller |
| Lock | device lock; the driver may take the controller lock inside | framework takes the controller lock directly |
| Ops source | `device_desc.ops` | `controller_desc.fops` |
| Teardown | BUSY while the device is open | BUSY while the direct open is active |

## Mixing with devices

- Whether long-term mixing is allowed is a driver policy: `fops->open` can call `elib_drv_controller_device_count(ctrl)` to check for bound devices and return `-ELIB_DRV_ERR_BUSY` if needed.
- Direct and device calls never interleave: both go through the same controller lock.
- If the controller has an "exclusive mode", implement the policy in `fops->open` instead of expecting the framework to do it.

## Example

```c
static int uart_open(elib_drv_controller_t *ctrl, uint32_t flags)
{
    (void)flags;
    return uart_set_baud(elib_drv_controller_priv(ctrl), 115200);
}

static int uart_read(elib_drv_controller_t *ctrl, void *buf, size_t len)
{
    return uart_ring_read(elib_drv_controller_priv(ctrl), buf, len);
}

static int uart_write(elib_drv_controller_t *ctrl, const void *buf, size_t len)
{
    return uart_ring_write(elib_drv_controller_priv(ctrl), buf, len);
}

static const elib_drv_controller_fops_t uart_fops = {
    uart_open, NULL, uart_read, uart_write, NULL
};

static elib_drv_controller_t uart_ctx;
static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "uart1", .ops = NULL, .fops = &uart_fops,
      .priv = &uart_hw, .ctx = &uart_ctx, .lock_timeout_ms = 100 },
};
```

```c
elib_drv_controller_t *uart = elib_drv_controller_find("uart1");

if (elib_drv_controller_open(uart, 0) == 0) {
    (void)elib_drv_controller_write(uart, "AT\r\n", 4);
    (void)elib_drv_controller_read(uart, buf, sizeof buf);
    (void)elib_drv_controller_close(uart);
}
```

## Pitfalls

- Calling `elib_drv_controller_lock` inside `fops->read/write` self-deadlocks (the framework already holds it).
- Calling device APIs from `fops` inverts the `device → controller` lock order and can deadlock.
- Forgetting to close a direct open leaves `deinit` / `unregister_controller` returning BUSY forever.
- With `fops == NULL` open/close still succeed and every I/O returns `NOT_SUPPORTED`; do not mistake that for "the controller is unusable".

## See also

- [Device open and I/O](usage_device_io.en.md)
- [Locking and concurrency](usage_locking.en.md)
- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Back to README](../README.en.md)
