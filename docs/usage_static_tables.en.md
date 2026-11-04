[简体中文](usage_static_tables.md) | **English**

# Static Descriptor Tables

Controller/device descriptors are written into flash at compile time, while the user declares one runtime context (`ctx`) per entry in RAM — this is the zero-dynamic-memory main path.

## When to use

- The set of devices is fixed at compile time (on-board EEPROM, sensors, UARTs, ...);
- You do not want to spend RAM on a registry, nor add/remove devices at run time;
- You want read-only descriptors placed in flash by the linker.

## Data model

| Object | Stored in | Written by | Contents |
|---|---|---|---|
| `controller_desc` | flash (`const`) | user | `name`, `ops`, `fops`, `priv`, `lock_timeout_ms` |
| `elib_drv_controller_t` | RAM (user declared) | framework | `desc`, `lock`, `device_count`, `open_count` |
| `device_desc` | flash (`const`) | user | `name`, `ops`, `priv`, `ctx`, `controller`, `flags`, `lock_timeout_ms` |
| `elib_drv_device_t` | RAM (user declared) | framework | `desc`, `lock`, `controller`, `open_count` |

`elib_drv_config_t` only keeps pointers to the two tables and their counts; the library never copies a static descriptor.

## Example

```c
#include "elib_drv.h"

static elib_drv_controller_t i2c1_ctx;
static elib_drv_device_t     eeprom_ctx;

static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "i2c1", .ops = NULL, .priv = &i2c1_hw, .ctx = &i2c1_ctx },
};
static const elib_drv_device_desc_t devs[] = {
    { .name = "eeprom", .ops = &eeprom_ops, .priv = &eeprom_info,
      .ctx = &eeprom_ctx, .controller = "i2c1",
      .lock_timeout_ms = 100 },
};

static const elib_drv_config_t cfg = {
    .controllers = ctrls, .controller_count = ELIB_DRV_ARRAY_SIZE(ctrls),
    .devices     = devs,  .device_count     = ELIB_DRV_ARRAY_SIZE(devs),
    .runtime     = NULL,
};
```

## Binding and validation

- A device's `controller` is a **name**, resolved once at `init` against the static controller table only; the binding is stored in the device `ctx.controller`.
- Every entry must bring its own `ctx`; its fields are maintained by the framework and must never be written by the caller.
- Names must be non-empty and unique within one table; controllers and devices are separate namespaces, so the same name may appear in both.
- When validation fails, `elib_drv_init` returns immediately: no lock is created and no `ops` is called.

| `elib_drv_init` return | Condition |
|---|---|
| `-ELIB_DRV_ERR_INVALID_PARAM` | empty name, `ctx == NULL`, table pointer NULL with a non-zero count |
| `-ELIB_DRV_ERR_ALREADY_EXIST` | duplicate name in the same table |
| `-ELIB_DRV_ERR_NOT_FOUND` | a device `controller` name does not exist |

## Memory cost

- 32-bit targets: `elib_drv_controller_t` 16 B, `elib_drv_device_t` 20 B (excluding the mutex itself, which the port owns).
- 64-bit targets: `elib_drv_controller_t` 24 B, `elib_drv_device_t` 32 B.
- Descriptors cost flash only; RAM holds one `ctx` per entry.

## Constraints

- Descriptor parameters are `const *` throughout the API, so code that writes them does not compile.
- Static entries cannot be unregistered: `elib_drv_unregister_*` returns `-ELIB_DRV_ERR_NOT_SUPPORTED`.
- A static device can only bind a static controller; only runtime-registered devices can bind runtime controllers.
- The lifetime of `name` and `priv` must cover the whole use period; the library stores pointers only.

## See also

- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Runtime registration](usage_runtime_tables.en.md)
- [Device open and I/O](usage_device_io.en.md)
- [Back to README](../README.en.md)
