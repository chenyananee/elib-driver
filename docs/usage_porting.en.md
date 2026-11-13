[简体中文](usage_porting.md) | **English**

# RTOS Porting

[Back to README](../README.en.md)

The framework is RTOS agnostic; the only thing to port is the four mutex
callbacks of `elib_drv_port_t`. `port == NULL` means single-threaded bare metal
and no locks are created.

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

- All four callbacks are required; a missing one makes init return
  `-INVALID_PARAM`.
- `mutex_lock` returns 0 on success and a negative value on timeout/failure;
  the framework maps it to `-ELIB_DRV_ERR_LOCK`.
- The framework does not allocate lock objects; the pointer returned by
  `mutex_create` is stored and passed to `mutex_destroy` during deinit.
- One lock is created per tree node; lock count equals node count.

## FreeRTOS

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

A zero-heap version uses `xSemaphoreCreateMutexStatic` with
`StaticSemaphore_t` arrays.

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

## Bare Metal / Single Thread

```c
elib_drv_init_tree(NULL, &tree, nodes, node_capacity);
```

A NULL port creates no locks, the lock helper always succeeds, and node lock
timeouts are ignored. Logging remains usable.

## Lock Rules

- One lock per node; `read/write/ioctl/open/close` call fops under the node lock.
- Device-to-controller forwarding uses `device lock -> controller lock`; the
  reverse order is forbidden.
- A node's fops must not call `elib_drv_*` I/O on the same node; the lock is not
  recursive.
- `elib_drv_node_lock()` is for grouping several low-level controller operations
  inside a driver; do not call framework I/O that locks the same node while
  holding it.
- All APIs are task-context only; do not call them from an ISR.

## Suggested Init Order

1. Call `elib_drv_init_tree()` before the scheduler starts or from a safe boot
   task.
2. After init, tasks obtain handles with `elib_drv_node_find()` and perform I/O.
3. Stop application work, then call `elib_drv_deinit()` after all node
   `open_count` values reach zero.
