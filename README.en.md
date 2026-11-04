[简体中文](README.md) | **English**

# elib-driver

A lightweight driver framework for MCU targets: three layers
(**manager / controller / device**), Linux `file_operations` style device
operations (open / close / read / write / ioctl), descriptors in flash, a
runtime context of a dozen bytes per device, RTOS mutexes through a port, and
an optional printf-style flow log.

- C99, zero dynamic memory; no hidden global state besides the manager
  singleton
- One public header `include/elib_drv.h`; usable without touching the library:
  no private headers, no capacity macros
- Static `const` descriptor tables plus an optional runtime slot table
  (user supplied cache, capacity derived automatically)
- Locking and logging are external interfaces; `NULL` port means bare metal
  with no locking and no log

Current version: **0.2.0** (`ELIB_DRV_VERSION`), single header `include/elib_drv.h`.

## Features

- **Clear layer split**: manager validates, creates locks, looks up and
  dispatches; controller is the hardware controller or driver class (shared
  resources and bus lock); device is an instance on a controller (the
  `file_operations` implementation)
- **Two registration styles**: compile-time `const` table (descriptors in
  flash) and runtime slot table (user cache, `elib_drv_register_*`); lookup
  and iteration are unified
- **Direct controller access**: a UART, a bare ADC or GPIO can bypass the
  device layer and use `elib_drv_controller_open/read/write/ioctl`; direct
  users and devices share the same controller lock, so calls never interleave
- **Automatic capacity**: runtime capacity = cache bytes / slot size, rounded
  down; static capacity is the entry count via `ELIB_DRV_ARRAY_SIZE()`, no
  `MAX_*` configuration macros
- **Open semantics**: reference counting plus optional exclusive access
  (`ELIB_DRV_DEV_F_EXCLUSIVE`); the handle is `elib_drv_device_t *`, there is
  no file descriptor object
- **RTOS agnostic**: `elib_drv_port_t` provides mutex create/destroy/lock/
  unlock, and the framework never allocates; `port == NULL` is the single
  threaded bare metal case
- **Optional flow log**: register `printf` or any `int (*)(const char *, ...)`
  callback; the framework logs init/register/unregister/open/close/failures
  **outside its locks**, never logs read/write/ioctl, and can be stripped with
  `-DELIB_DRV_LOG_ENABLE=0`

## Usage Docs

| Flow | Doc |
|------|-----|
| Lifecycle and initialization | [docs/usage_lifecycle.en.md](docs/usage_lifecycle.en.md) |
| Static descriptor tables | [docs/usage_static_tables.en.md](docs/usage_static_tables.en.md) |
| Runtime registration | [docs/usage_runtime_tables.en.md](docs/usage_runtime_tables.en.md) |
| Device open and I/O | [docs/usage_device_io.en.md](docs/usage_device_io.en.md) |
| Direct controller access | [docs/usage_controller_direct.en.md](docs/usage_controller_direct.en.md) |
| Locking and concurrency | [docs/usage_locking.en.md](docs/usage_locking.en.md) |
| Flow logging | [docs/usage_logging.en.md](docs/usage_logging.en.md) |
| RTOS porting | [docs/usage_porting.en.md](docs/usage_porting.en.md) |
| Building and testing | [docs/usage_build_test.en.md](docs/usage_build_test.en.md) |

## Layers and call path

```
user (device path): elib_drv_open/close/read/write/ioctl(dev)
        │
        ▼
manager facade: validation → take device lock → call ops → release → log
        │
        ▼
device ops (file_operations): open/close/read/write/ioctl(dev, ...)
        │            │
        │            └─ shared bus: elib_drv_controller_lock(ctrl, timeout)
        ▼
controller priv (registers/bus/peripheral) → hardware

user (direct path): elib_drv_controller_open/close/read/write/ioctl(ctrl)
        │
        ▼
manager facade: take controller lock → call controller fops → release → log
        │
        ▼
controller priv → hardware (shares the same controller lock as devices)
```

| Layer | Role | Constant (flash) | Mutable (RAM, user provided) |
|---|---|---|---|
| manager | registry, dispatch, log fan-out | — | singleton (`initialized`/`port`/tables/`log`) |
| controller | hardware controller / driver class and shared resources | `controller_desc` (name/ops/fops/priv) | static: `elib_drv_controller_t` (device count + direct open count); runtime: inline in slot |
| device | instance on a controller | `device_desc` (name/ops/priv/binding/flags/timeout) | static: `elib_drv_device_t`; runtime: inline in slot |

## Memory model

### Static table: descriptors in flash, contexts in RAM

```c
static elib_drv_controller_t i2c1_ctx;
static elib_drv_device_t     eeprom_ctx;

static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "i2c1", .ops = NULL, .priv = &i2c1_hw, .ctx = &i2c1_ctx },
};
static const elib_drv_device_desc_t devs[] = {
    { .name = "eeprom", .ops = &eeprom_ops, .priv = &eeprom_info,
      .ctx = &eeprom_ctx, .controller = "i2c1" },
};

static const elib_drv_config_t cfg = {
    .controllers = ctrls, .controller_count = ELIB_DRV_ARRAY_SIZE(ctrls),
    .devices     = devs,  .device_count     = ELIB_DRV_ARRAY_SIZE(devs),
    .runtime     = NULL,                     /* static table only */
};

elib_drv_init(&port, &cfg);                  /* port = NULL for bare metal */
```

- Descriptors are read-only: every descriptor parameter is `const *`, so code
  that writes them does not compile.
- Every entry points at its own `ctx`; the framework owns those fields and the
  caller must never write them.
- RAM cost on a 32-bit target: 20 B per device context, 16 B per controller
  (32/24 B on 64-bit).
- `port`, the descriptor tables and the runtime cache must outlive the manager
  (static storage is the normal choice); the `config` struct itself may live on
  the stack.

### Runtime table: user cache, automatic capacity

```c
static elib_drv_controller_slot_t rt_ctrls[2];
static elib_drv_device_slot_t     rt_devs[8];

static const elib_drv_runtime_t runtime = {
    .controllers = { rt_ctrls, sizeof rt_ctrls },
    .devices     = { rt_devs,  sizeof rt_devs  },
};

/* cfg.runtime = &runtime; then init ... */

elib_drv_device_desc_t d = {
    .name = "sensor3", .ops = &sensor_ops, .controller = "i2c1",
};
elib_drv_device_t *dev = NULL;
elib_drv_register_device(&d, &dev);      /* add at runtime */
/* ... */
elib_drv_unregister_device(dev);         /* remove, slot is reusable */
```

- One slot = the same descriptor plus its context; capacity = aligned bytes /
  `sizeof(slot)`, rounded down, the remainder is dropped.
- The framework aligns the cache base itself.
- Without a cache (`runtime == NULL` or capacity 0) `elib_drv_register_*`
  returns `-ELIB_DRV_ERR_NO_SLOT`.
- Registration copies the caller descriptor; `desc.name` must outlive the
  registration (used by lookup) while `desc.controller` is only read during the
  call.
- Static entries cannot be unregistered (`-ELIB_DRV_ERR_NOT_SUPPORTED`);
  static devices may only bind static controllers, runtime devices may bind
  either.
- A runtime handle is invalid immediately after unregistration: unregistering it
  again returns `-ELIB_DRV_ERR_INVALID_PARAM`, and an old handle must never be
  used after its slot has been reused.

### Registration combinations

| controller \ device | static device | runtime device |
|---|---|---|
| static controller | supported: bound by name at `init` | supported: bound by name in `register_device` |
| runtime controller | not supported: `init` only searches the static table and returns `-ELIB_DRV_ERR_NOT_FOUND` | supported: `register_controller` first, then `register_device` |

- A static device's `controller` is resolved once against the static table in `elib_drv_init`; there is no deferred binding/rebind API, and `controller = NULL` leaves it unbound.
- A runtime device may bind a static or a runtime controller; a static device cannot bind a runtime controller.

## Quick start

```c
#include "elib_drv.h"
#include <stdio.h>
#include <string.h>

/* ---- 1. driver implementation: file_operations style ---- */
static int eeprom_open(elib_drv_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    return 0;
}

static int eeprom_close(elib_drv_device_t *dev)
{
    (void)dev;
    return 0;
}

static int eeprom_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    (void)dev;
    memset(buf, 0, len);
    return (int)len;
}

static int eeprom_write(elib_drv_device_t *dev, const void *buf, size_t len)
{
    (void)dev;
    (void)buf;
    return (int)len;
}

static int eeprom_ioctl(elib_drv_device_t *dev, uint32_t cmd, void *arg)
{
    (void)dev;
    (void)cmd;
    (void)arg;
    return 0;
}

static const elib_drv_ops_t eeprom_ops = {
    eeprom_open, eeprom_close, eeprom_read, eeprom_write, eeprom_ioctl
};

/* ---- 2. contexts + const descriptor table ---- */
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

/* ---- 3. boot ---- */
int app_start(void)
{
    elib_drv_device_t *dev;
    unsigned char buf[16];
    int rc;

    elib_drv_set_log(printf);            /* optional: printf directly */
    rc = elib_drv_init(NULL, &cfg);      /* bare metal; see port section */
    if (rc != 0) {
        return rc;
    }
    dev = elib_drv_device_find("eeprom");
    if (dev == NULL) {
        return -1;
    }
    (void)elib_drv_open(dev, 0);
    (void)elib_drv_read(dev, buf, sizeof buf);
    (void)elib_drv_ioctl(dev, ELIB_DRV_IOC(1, 0), NULL);
    (void)elib_drv_close(dev);
    return 0;
}
```

## Direct controller access (no device layer)

A UART, a bare ADC or GPIO often needs no device layer. Put a `fops` table on
the controller descriptor and use it with the same file_operations flavour:

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

static int uart_ioctl(elib_drv_controller_t *ctrl, uint32_t cmd, void *arg)
{
    return uart_control(elib_drv_controller_priv(ctrl), cmd, arg);
}

static const elib_drv_controller_fops_t uart_fops = {
    uart_open, NULL, uart_read, uart_write, uart_ioctl
};

static elib_drv_controller_t uart_ctx;
static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "uart1", .ops = NULL, .fops = &uart_fops,
      .priv = &uart_hw, .ctx = &uart_ctx, .lock_timeout_ms = 100 },
};
```

```c
elib_drv_controller_t *uart = elib_drv_controller_find("uart1");

elib_drv_controller_open(uart, 0);
elib_drv_controller_write(uart, "AT\r\n", 4);
elib_drv_controller_read(uart, buf, sizeof buf);
elib_drv_controller_ioctl(uart, ELIB_DRV_IOC(3, 1), &baud);
elib_drv_controller_close(uart);
```

Rules:

- Direct open/close is reference counted and **independent** from device
  opens; while a direct open is pending, `elib_drv_deinit` and
  `elib_drv_unregister_controller` return `-ELIB_DRV_ERR_BUSY`.
- Every direct call takes the **controller lock** (`desc.lock_timeout_ms`,
  0 = forever) — the same lock a device driver takes for a shared bus, so
  direct users and devices never interleave a transfer.
- `fops` runs under that lock: do not call `elib_drv_controller_lock` again
  and do not call `elib_drv_device_*` (it would invert the
  device -> controller order).
- A NULL `fops` (or a NULL hook) keeps open/close usable (counting only) and
  makes the matching I/O return `-ELIB_DRV_ERR_NOT_SUPPORTED`.
- Whether direct users and devices may coexist long term is the driver's
  policy: `fops->open` can check `elib_drv_controller_device_count(ctrl)` and
  return `-ELIB_DRV_ERR_BUSY` when a device is bound.

## Core concepts

### Return convention

Every function returns `int`: `0` or a positive value on success (bytes for
read/write, driver defined for ioctl), a negative value on failure equal to
`-(elib_drv_err_t)`. Driver private errors start at
`-ELIB_DRV_ERR_DRIVER_BASE - n` and pass through unchanged.

| Error | Meaning |
|---|---|
| `ELIB_DRV_ERR_INVALID_PARAM` | `NULL`, empty name, inconsistent entry, incomplete port |
| `ELIB_DRV_ERR_NOT_INITIALIZED` | `elib_drv_init` not called |
| `ELIB_DRV_ERR_ALREADY_INIT` | double init |
| `ELIB_DRV_ERR_ALREADY_EXIST` | duplicate name in the static or runtime table |
| `ELIB_DRV_ERR_NOT_FOUND` | controller name not found |
| `ELIB_DRV_ERR_NO_SLOT` | runtime table absent or full |
| `ELIB_DRV_ERR_BUSY` | deinit with an open device, a directly opened controller, or while init is still running; unregister_controller with an open device/direct open, a controller still bound to devices, or itself from its own init |
| `ELIB_DRV_ERR_NOT_OPEN` | read/write/ioctl before open |
| `ELIB_DRV_ERR_NOT_SUPPORTED` | ops/fops pointer is `NULL`; or unregister a static entry |
| `ELIB_DRV_ERR_LOCK` | lock creation failed / lock timeout |
| `ELIB_DRV_ERR_TIMEOUT` / `_IO` | ordinary driver errors |

### Open semantics

- Open is reference counted: three opens call `ops->open` once; `ops->close`
  runs when the last close drops the count to zero. Direct controller
  open/close uses the same rule but its count is independent from devices.
- Close decrements the count first and only calls `ops->close` at zero; if
  `ops->close` returns a negative value the count is already released, the
  framework returns that error and logs it (the device stays closed).
- `ELIB_DRV_DEV_F_EXCLUSIVE`: a second open returns `-BUSY`.
- read/write/ioctl require an opened device or controller (`-NOT_OPEN`) and
  run under the matching lock.
- The device lock timeout is `device_desc.lock_timeout_ms`; direct controller
  calls use `controller_desc.lock_timeout_ms`; `0` means wait forever and a
  failure returns `-ELIB_DRV_ERR_LOCK`.

### Controller lock

When several devices share a bus, the driver takes the controller lock inside
its ops:

```c
static int sensor_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    elib_drv_controller_t *bus = elib_drv_device_controller(dev);
    int rc = elib_drv_controller_lock(bus, ELIB_DRV_WAIT_FOREVER);

    if (rc != 0) {
        return rc;
    }
    rc = i2c_burst_read(elib_drv_controller_priv(bus), dev, buf, len);
    (void)elib_drv_controller_unlock(bus);
    return rc;
}
```

## Optional flow log

```c
elib_drv_set_log(printf);                 /* simplest: printf directly */

/* or your own UART/RTT logger */
#include <stdarg.h>
#include <stdio.h>

static int uart_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    return 0;
}
elib_drv_set_log(uart_log);
```

- The framework always calls the callback **after releasing its locks**.
- The callback may be invoked concurrently from several tasks (different
  devices opening at the same time) and must be thread safe.
- It must **not** call any `elib_drv_*` function (recursion/reentrancy).
- Log points: init start/success/failure, controller init/deinit and failures,
  register/unregister and their failure reasons, device open/close and
  failures, **direct controller open/close and failures**, deinit busy, lock
  timeouts. read/write/ioctl are never logged.
- Messages are `"[elib_drv] I|W|E: ..."` and only use `%s/%u/%d`.
- `elib_drv_set_log(NULL)` disables logging; `deinit` keeps the callback.
- `-DELIB_DRV_LOG_ENABLE=0` strips every log call and format string at compile
  time (enabled by default).

## API

```c
/* manager */
int elib_drv_init(const elib_drv_port_t *port, const elib_drv_config_t *config);
int elib_drv_deinit(void);
elib_drv_controller_t *elib_drv_controller_find(const char *name);
elib_drv_device_t     *elib_drv_device_find(const char *name);
int elib_drv_device_foreach(int (*fn)(elib_drv_device_t *, void *), void *arg);

/* runtime registration */
int elib_drv_register_controller(const elib_drv_controller_desc_t *, elib_drv_controller_t **out);
int elib_drv_unregister_controller(elib_drv_controller_t *);
int elib_drv_register_device(const elib_drv_device_desc_t *, elib_drv_device_t **out);
int elib_drv_unregister_device(elib_drv_device_t *);

/* user I/O (device) */
int elib_drv_open(elib_drv_device_t *, uint32_t flags);
int elib_drv_close(elib_drv_device_t *);
int elib_drv_read(elib_drv_device_t *, void *buf, size_t len);
int elib_drv_write(elib_drv_device_t *, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_device_t *, uint32_t cmd, void *arg);

/* direct controller access (no device layer) */
int elib_drv_controller_open(elib_drv_controller_t *, uint32_t flags);
int elib_drv_controller_close(elib_drv_controller_t *);
int elib_drv_controller_read(elib_drv_controller_t *, void *buf, size_t len);
int elib_drv_controller_write(elib_drv_controller_t *, const void *buf, size_t len);
int elib_drv_controller_ioctl(elib_drv_controller_t *, uint32_t cmd, void *arg);

/* driver side */
const char *elib_drv_controller_name(const elib_drv_controller_t *);
void       *elib_drv_controller_priv(const elib_drv_controller_t *);
unsigned    elib_drv_controller_open_count(const elib_drv_controller_t *);
unsigned    elib_drv_controller_device_count(const elib_drv_controller_t *);
int         elib_drv_controller_lock(elib_drv_controller_t *, uint32_t timeout_ms);
int         elib_drv_controller_unlock(elib_drv_controller_t *);
const char *elib_drv_device_name(const elib_drv_device_t *);
void       *elib_drv_device_priv(const elib_drv_device_t *);
elib_drv_controller_t *elib_drv_device_controller(const elib_drv_device_t *);
unsigned    elib_drv_device_open_count(const elib_drv_device_t *);

/* flow log */
typedef int (*elib_drv_log_fn)(const char *fmt, ...);
int elib_drv_set_log(elib_drv_log_fn fn);
```

`ioctl` commands are usually built with `ELIB_DRV_IOC(group, nr)` and declared
in the driver's own header, never in the library.

## Porting to an RTOS

The only piece to port is `elib_drv_port_t`:

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

All four callbacks are required; `mutex_lock` returns 0 on success and a
negative value on failure (mapped to `-ELIB_DRV_ERR_LOCK`); `timeout_ms` is
`ELIB_DRV_WAIT_FOREVER` or milliseconds. **The framework never allocates**,
so the port owns the mutex storage (heap or static pool).

### FreeRTOS

```c
static void *port_create(const char *name)
{
    (void)name;
    return (void *)xSemaphoreCreateMutex();
}

static void port_destroy(void *m)
{
    vSemaphoreDelete((SemaphoreHandle_t)m);
}

static int port_lock(void *m, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == ELIB_DRV_WAIT_FOREVER)
                           ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTake((SemaphoreHandle_t)m, ticks) == pdTRUE) ? 0 : -1;
}

static int port_unlock(void *m)
{
    return (xSemaphoreGive((SemaphoreHandle_t)m) == pdTRUE) ? 0 : -1;
}

static const elib_drv_port_t port = {
    port_create, port_destroy, port_lock, port_unlock
};
```

For the no-heap variant use `xSemaphoreCreateMutexStatic` with an array of
`StaticSemaphore_t`.

### CMSIS-RTOS2

```c
static void *port_create(const char *name)
{
    (void)name;
    return (void *)osMutexNew(NULL);
}

static void port_destroy(void *m)
{
    (void)osMutexDelete((osMutexId_t)m);
}

static int port_lock(void *m, uint32_t timeout_ms)
{
    uint32_t ticks = (timeout_ms == ELIB_DRV_WAIT_FOREVER)
                         ? osWaitForever : timeout_ms;
    return (osMutexAcquire((osMutexId_t)m, ticks) == osOK) ? 0 : -1;
}

static int port_unlock(void *m)
{
    return (osMutexRelease((osMutexId_t)m) == osOK) ? 0 : -1;
}
```

### Bare metal / single threaded

`elib_drv_init(NULL, &cfg)` creates no locks, takes no locks, uses no extra
RAM, while the flow log still works independently.

## Concurrency and lock rules

| Lock | Protects | Taken by | Timeout |
|---|---|---|---|
| device lock | open count, device state, ops serialisation | framework | `device_desc.lock_timeout_ms` (0 = forever) |
| controller lock | bus / shared registers + direct fops serialisation | framework (direct API) and the driver (inside device ops) | direct: `controller_desc.lock_timeout_ms`; driver's choice |

- `device → controller` nesting is allowed, the reverse is forbidden. Direct
  calls take only the controller lock and never nest a device lock. Ops/fops
  of the same device/controller must not call the matching
  `elib_drv_*open/close/read/write/ioctl` again (a non recursive mutex would
  self-deadlock).
- `init` / `deinit` / `register_*` / `unregister_*` are startup or discovery
  time operations, never concurrent with I/O; `find` / `foreach` are read only
  and can run concurrently with I/O, but never with `register_*` /
  `unregister_*` (they would hand out dangling pointers); a `foreach` callback
  must not register or unregister.
- All APIs are task context only; ISRs must use the driver's own mechanisms.
- Unregistering a static entry returns `-NOT_SUPPORTED`; a runtime entry that
  is still open, or a directly opened controller, returns `-BUSY`.
- Logs are printed outside the locks; the callback must not call `elib_drv_*`
  and must be thread safe.

## Building and testing

No build system: add `include/` to the include path and `src/*.c` to the
project.

```bash
# library
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_device.c

# functional regression (public header only)
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv && ./t_drv

# concurrency check (POSIX, not part of the default build)
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt && ./t_mt

# log stripped build
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c
```

## Layout

```
include/elib_drv.h       the only public header: errors/descriptors/contexts/port/API/log
src/elib_drv_internal.h  private singleton, lock helpers, log macro
src/elib_drv_manager.c   init/deinit/validation/tables/registration/lookup
src/elib_drv_device.c    open/close/read/write/ioctl/accessors/controller lock
test/test_elib_drv.c     functional + dual table + log + lock discipline
test/test_elib_drv_mt.c  POSIX multi thread check (-pthread)
docs/usage_*.md          nine flow docs (each with a .en.md English version)
```

## License

MIT License
