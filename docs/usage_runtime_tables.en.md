[简体中文](usage_runtime_tables.md) | **English**

# Runtime Registration

Besides the static tables, the user may hand the manager a byte cache as a slot pool, then add controllers/devices at run time with `elib_drv_register_*` and remove them with `elib_drv_unregister_*`, reusing slots.

## When to use

- Devices are discovered after boot (bus scan, hot plug, configuration-driven);
- Temporary devices are registered on demand and removed when done;
- The RAM cost must be exactly the cache you supply, with no `MAX_*` macros.

## Slot pools and capacity

```text
cache base ──► aligned up to pointer size (padding discarded)
usable bytes = bytes - alignment padding
capacity     = usable bytes / sizeof(slot), rounded down
slot         = descriptor + the ctx of that descriptor
```

- `elib_drv_runtime_t` takes one `mem + bytes` pair for `controllers` and one for `devices`; `mem == NULL` with `bytes == 0` means no runtime slots of that kind.
- Alignment and zeroing are done by `elib_drv_init`; you do not manage alignment and must not place other data in that memory.
- With zero capacity, every `elib_drv_register_*` returns `-ELIB_DRV_ERR_NO_SLOT`.

## Registration flow

1. `elib_drv_register_controller(desc, &out)`:
   - check initialization, `desc`/`name` validity, and that the name is free in both tables;
   - find a slot whose `ctx.desc == NULL`, clear it and **copy the descriptor by value**;
   - override `desc.ctx` with the slot's inline ctx, create the lock (when `port != NULL`);
   - call `desc->ops->init` (it may register child devices); on success `out` receives the handle.
2. `elib_drv_register_device(desc, &out)`:
   - resolve `desc->controller` by name first (static and runtime tables are both searched);
   - allocate a free slot, copy the descriptor, create the lock, then increment the controller's `device_count`;
   - `out` may be `NULL` to register without taking a handle.
3. Ordering: a controller must exist before a device binds it; a device may bind a static or a runtime controller, while a static device can only bind a static controller.

## Unregistration rules

| Operation | Condition | Return |
|---|---|---|
| unregister a static entry | always | `-ELIB_DRV_ERR_NOT_SUPPORTED` |
| `unregister_controller` | direct `open_count != 0` | `-ELIB_DRV_ERR_BUSY` |
| `unregister_controller` | devices still bound (`device_count != 0`) | `-ELIB_DRV_ERR_BUSY` |
| `unregister_device` | `open_count != 0` | `-ELIB_DRV_ERR_BUSY` |
| `unregister_*` | the handle is not a runtime slot | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `unregister_*` | unregistering an already released slot again | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `unregister_controller` | unregistering itself from its own `ops->init` | `-ELIB_DRV_ERR_BUSY` |

- Unregistering a controller calls its `ops->deinit`; when `ops->deinit` returns an error the slot is **still released** and the error is returned to the caller.
- Unregistering a device decrements the controller's `device_count`; the cleared slot can be reused by a later registration.
- `desc.name` must live until unregistration (lookup and logging read it); `desc.controller` is only used to resolve during the registration call.

## Failure cleanup

| Failure point | Framework cleanup |
|---|---|
| controller duplicate / no free slot / lock creation | slot untouched or cleared, `device_count`/`open_count` unchanged |
| controller `ops->init` fails | release every runtime device registered during `init` (whatever controller it bound), close its direct open, destroy the lock, clear the slot |
| device duplicate / unknown controller / no free slot / lock creation | slot untouched, the controller's `device_count` unchanged |

## Example

```c
static elib_drv_controller_slot_t rt_ctrls[2];
static elib_drv_device_slot_t     rt_devs[8];

static const elib_drv_runtime_t runtime = {
    .controllers = { rt_ctrls, sizeof rt_ctrls },
    .devices     = { rt_devs,  sizeof rt_devs  },
};

static const elib_drv_config_t cfg = {
    .controllers = NULL, .controller_count = 0,
    .devices     = NULL, .device_count     = 0,
    .runtime     = &runtime,
};

void add_sensor(void)
{
    elib_drv_device_desc_t d = {
        .name = "sensor3", .ops = &sensor_ops, .controller = "i2c1",
    };
    elib_drv_device_t *dev = NULL;

    if (elib_drv_register_device(&d, &dev) == 0) {
        (void)elib_drv_unregister_device(dev);   /* the slot becomes reusable */
    }
}
```

## Constraints

- The slot cache is user supplied and must outlive the registration; the library never allocates or grows it.
- Runtime and static tables share one lookup namespace, so a duplicate name is rejected at registration.
- `elib_drv_register_*` / `unregister_*` are startup or discovery-time calls, never concurrent with I/O.
- A handle is invalid immediately after unregistration: unregistering it again returns `-ELIB_DRV_ERR_INVALID_PARAM`; an old handle must not be used once its slot is reused; `find`/`foreach` must not run concurrently with unregistration.

## See also

- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Static descriptor tables](usage_static_tables.en.md)
- [Locking and concurrency](usage_locking.en.md)
- [Back to README](../README.en.md)
