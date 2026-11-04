[简体中文](usage_porting.md) | **English**

# RTOS Porting

The framework is RTOS agnostic; the only thing to port is the four mutex callbacks of `elib_drv_port_t`. With `port == NULL` it is single-threaded bare metal.

## The interface to implement

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

| Callback | Contract |
|---|---|
| `mutex_create` | return the mutex handle; `NULL` on failure (the framework maps it to `-ELIB_DRV_ERR_LOCK`) |
| `mutex_destroy` | release the handle; called only when the framework no longer needs it |
| `mutex_lock` | 0 on success; negative on timeout/failure (mapped to `-ELIB_DRV_ERR_LOCK`) |
| `mutex_unlock` | 0 on success; negative on failure (the return value is ignored) |

## Timeouts and return values

- `timeout_ms` may be `ELIB_DRV_WAIT_FOREVER` (`UINT32_MAX`); the port should treat it as "wait forever".
- Device and direct controller lock timeouts come from `device_desc.lock_timeout_ms` and `controller_desc.lock_timeout_ms`; the framework has already mapped `0` to `ELIB_DRV_WAIT_FOREVER`.
- A negative `mutex_lock` return is always mapped to `-ELIB_DRV_ERR_LOCK`; do not return framework error codes from the port.
- The `mutex_unlock` return value is ignored, but the implementation should still return 0/negative correctly.

## Storage and zero heap

- The framework never allocates; the port owns the mutex storage, which may be heap or a static pool.
- A zero-heap project uses the RTOS static creation API (for example FreeRTOS `xSemaphoreCreateMutexStatic` with a `StaticSemaphore_t` array) and hands the handle to the framework.
- `name` is for debugging/logging only and may be `NULL` or ignored.

## FreeRTOS

```c
#include "FreeRTOS.h"
#include "semphr.h"

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

Zero-heap variant: replace `xSemaphoreCreateMutex` with `xSemaphoreCreateMutexStatic` (plus a `StaticSemaphore_t` array).

## CMSIS-RTOS2

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

## Bare metal

`elib_drv_init(NULL, &cfg)`: no locks created or taken and no extra RAM, while the log still works. Every API is then a synchronous call and concurrency does not apply.

## Checklist

- All four callbacks are present and never return undefined values;
- `mutex_lock` maps `ELIB_DRV_WAIT_FOREVER` to the RTOS infinite wait;
- millisecond-to-tick conversion uses the RTOS macro (`pdMS_TO_TICKS`, ...);
- `mutex_create` returns `NULL` on failure without leaking already created resources;
- no `mutex_destroy` callback is used after framework deinit/unregistration;
- mutexes may be statically allocated by the port to satisfy zero-heap;
- port callbacks must not call `elib_drv_*`.

## See also

- [Locking and concurrency](usage_locking.en.md)
- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Flow logging](usage_logging.en.md)
- [Back to README](../README.en.md)
