**简体中文** | [English](usage_porting.en.md)

# RTOS 移植

框架与 RTOS 解耦，唯一需要移植的是 `elib_drv_port_t` 的四个互斥量回调；`port == NULL` 即单线程裸机。

## 需要实现的接口

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

| 回调 | 约定 |
|---|---|
| `mutex_create` | 返回互斥量句柄；失败返回 `NULL`（框架映射为 `-ELIB_DRV_ERR_LOCK`） |
| `mutex_destroy` | 释放句柄；只在框架不再使用它之后调用 |
| `mutex_lock` | 成功返回 0；超时/失败返回负值（框架映射为 `-ELIB_DRV_ERR_LOCK`） |
| `mutex_unlock` | 成功返回 0；失败返回负值（框架忽略返回值） |

## 超时与返回值

- `timeout_ms` 可能是 `ELIB_DRV_WAIT_FOREVER`（`UINT32_MAX`），port 应把它当“永久等待”处理。
- 设备锁与直连 controller 锁的超时分别来自 `device_desc.lock_timeout_ms` 与 `controller_desc.lock_timeout_ms`，`0` 在框架内已映射为 `ELIB_DRV_WAIT_FOREVER`。
- `mutex_lock` 的负返回值统一映射为 `-ELIB_DRV_ERR_LOCK`；不要在 port 里返回框架错误码。
- `mutex_unlock` 的返回值被忽略，但实现仍应正确返回 0/负值。

## 存储与零堆

- 框架自身不做任何分配；互斥量的存储由 port 负责，堆或静态池都可以。
- 零堆工程用 RTOS 的静态创建接口（如 FreeRTOS 的 `xSemaphoreCreateMutexStatic` + `StaticSemaphore_t` 数组），把句柄交给框架。
- `name` 只用于调试/日志，可以为 `NULL` 或忽略。

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

零堆版本：把 `xSemaphoreCreateMutex` 换成 `xSemaphoreCreateMutexStatic`（配 `StaticSemaphore_t` 数组）。

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

## 裸机

`elib_drv_init(NULL, &cfg)`：不建锁、不加锁、零额外 RAM；日志仍可独立使用。此时所有 API 都是同步调用，不需要考虑并发。

## 检查清单

- 四个回调齐全且都不返回未定义值；
- `mutex_lock` 把 `ELIB_DRV_WAIT_FOREVER` 映射为 RTOS 的永久等待；
- 毫秒到 tick 的换算使用 RTOS 自己的宏（`pdMS_TO_TICKS` 等）；
- `mutex_create` 失败返回 `NULL`，并且不会泄漏已经创建的资源；
- `mutex_destroy` 在框架 `deinit`/注销后不会再被回调使用；
- 互斥量可由 port 静态分配，满足零堆要求；
- 不要在 port 回调里调用 `elib_drv_*`。

## 相关

- [锁、并发与超时](usage_locking.md)
- [生命周期与初始化](usage_lifecycle.md)
- [流程日志](usage_logging.md)
- [返回 README](../README.md)
