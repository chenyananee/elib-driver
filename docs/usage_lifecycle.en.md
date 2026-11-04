[简体中文](usage_lifecycle.md) | **English**

# Lifecycle and Initialization

The manager is a singleton: `elib_drv_init` wires the port, the static tables and the runtime slot pools in one pass, while `elib_drv_deinit` tears everything down in a fixed order. This page covers both flows, who rolls back each failure point, and the BUSY conditions of deinit.

## When to use

- Initialize the driver framework at boot and register the static controllers/devices;
- Understand what state hardware and memory are left in when `elib_drv_init` fails;
- Tear the framework down safely with `elib_drv_deinit` before shutdown or restart.

## Flow diagram

```text
elib_drv_init(port, cfg)
  ├─ already initialized? ── yes → -ELIB_DRV_ERR_ALREADY_INIT
  ├─ port validation (NULL, or all four callbacks present)
  ├─ config validation (table pointers, names, ctx, duplicates, controller refs)
  ├─ clear the manager singleton (keep the log callback)
  ├─ align/zero the runtime slot pools, derive capacity
  ├─ write the static controller/device ctx, bind devices by name
  ├─ create one lock per entry when port != NULL
  ├─ initialized = 1
  ├─ call ops->init per controller (it may register its own devices)
  └─ any failure → roll back and return that error
```

## Initialization steps

1. **Idempotence check**: when `elib_drv_g_manager.initialized` is 1, return `-ELIB_DRV_ERR_ALREADY_INIT` without touching any state.
2. **Port validation**: `NULL` means bare metal; otherwise `mutex_create/destroy/lock/unlock` must all be present, or `-INVALID_PARAM`.
3. **Config validation**: see the error table below. Every check runs before the manager is modified, so a failure leaves the framework state untouched.
4. **Wire the static tables**: each controller/device `ctx` is cleared, then filled with the `desc` pointer and initial counters; a device's `controller` name is resolved against the static controller table only.
5. **Create locks**: with `port != NULL`, controllers first and then devices, using the descriptor `name` as the lock name; one failure rolls everything back.
6. **Controller init**: `initialized` is already set, so `ops->init` may call `elib_drv_register_device` and friends; a negative return from `ops->init` is a failure.

## Failure rollback

| Failure point | What the framework cleans up | What the caller sees |
|---|---|---|
| Port validation | manager untouched | `-ELIB_DRV_ERR_INVALID_PARAM` |
| Config validation | manager untouched | `INVALID_PARAM` / `ALREADY_EXIST` / `NOT_FOUND` |
| Lock creation | all created locks destroyed, static ctx cleared, no `ops` called | `-ELIB_DRV_ERR_LOCK` |
| Controller `ops->init` | runtime devices and runtime controllers released, all locks destroyed; controllers that **did** init are `deinit`ed, the one that **failed is not** | that `ops->init` return value |
| Success | none | `0` |

> A failing `ops->init` must clean up its own half-done initialization, including any direct open it made; the framework only releases the runtime devices it registered and destroys its lock.

## Deinit flow

`elib_drv_deinit` performs four BUSY checks and then runs the teardown:

| Check | Condition |
|---|---|
| Static device | `open_count != 0` |
| Runtime device slot | `desc != NULL && open_count != 0` |
| Static controller | `open_count != 0` (direct) |
| Runtime controller slot | `desc != NULL && open_count != 0` (direct) |

The teardown order is fixed:

1. Release static devices (calling `ops->close` when needed);
2. Release runtime device slots;
3. Release runtime controller slots (close direct, call `ops->deinit`, destroy the lock);
4. Release static controllers (same as above);
5. `memset` the manager but **keep the log callback**, and return the first `ops->deinit` error.

Deinit releases everything it can: even when one `ops->deinit` fails, the remaining controllers are still released and the caller receives the first error code.

## Error codes

| Error code | Condition |
|---|---|
| `ELIB_DRV_ERR_ALREADY_INIT` | repeated `elib_drv_init` |
| `ELIB_DRV_ERR_INVALID_PARAM` | `cfg == NULL`; a table pointer is NULL with a non-zero count; empty name; `ctx == NULL`; incomplete port |
| `ELIB_DRV_ERR_ALREADY_EXIST` | duplicate controller or device name in a table |
| `ELIB_DRV_ERR_NOT_FOUND` | a device `controller` name does not resolve |
| `ELIB_DRV_ERR_LOCK` | `mutex_create` returned `NULL` |
| `ELIB_DRV_ERR_NOT_INITIALIZED` | any API used before init, or deinit twice |
| `ELIB_DRV_ERR_BUSY` | deinit while a device or a direct controller open is still active |
| Controller `ops->init` error | passed through unchanged (negative) |

## Example

```c
#include "elib_drv.h"

static elib_drv_controller_t i2c1_ctx;
static elib_drv_device_t     eeprom_ctx;

static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "i2c1", .ops = NULL, .priv = NULL, .ctx = &i2c1_ctx },
};
static const elib_drv_device_desc_t devs[] = {
    { .name = "eeprom", .ops = &eeprom_ops, .priv = NULL,
      .ctx = &eeprom_ctx, .controller = "i2c1" },
};
static const elib_drv_config_t cfg = {
    .controllers = ctrls, .controller_count = ELIB_DRV_ARRAY_SIZE(ctrls),
    .devices     = devs,  .device_count     = ELIB_DRV_ARRAY_SIZE(devs),
    .runtime     = NULL,
};

int app_boot(void)
{
    int rc = elib_drv_init(NULL, &cfg);   /* NULL port = bare metal */

    if (rc != 0) {
        return rc;                        /* the framework rolled back already */
    }
    elib_drv_device_t *dev = elib_drv_device_find("eeprom");
    if (dev == NULL) {
        (void)elib_drv_deinit();
        return -1;
    }
    return 0;
}

void app_shutdown(void)
{
    /* close every device and direct controller first, otherwise -BUSY */
    (void)elib_drv_deinit();
}
```

## Notes

- `init` may be retried after a failure; `init` after `deinit` works too (the manager was cleared).
- `elib_drv_set_log` may be called before `init`; `deinit` does not clear the callback.
- A controller's `ops->init` may register runtime devices, but it must not call `elib_drv_deinit`.
- The deinit BUSY checks take no locks, which assumes `init/deinit` never run concurrently with I/O (see [Locking and Concurrency](usage_locking.en.md)).

## See also

- [Static descriptor tables](usage_static_tables.en.md)
- [Runtime registration](usage_runtime_tables.en.md)
- [Locking and concurrency](usage_locking.en.md)
- [Back to README](../README.en.md)
