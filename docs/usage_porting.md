**简体中文** | [English](usage_porting.en.md)

# RTOS 移植

[返回 README](../README.md)

框架与 RTOS 解耦，唯一需要移植的是 `elib_drv_port_t` 四个互斥量回调。
`port == NULL` 即单线程裸机，不创建锁。

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

- 四个回调必须同时提供；缺一个 init 返回 `-INVALID_PARAM`。
- `mutex_lock` 成功返回 0，超时/失败返回负数；框架映射为
  `-ELIB_DRV_ERR_LOCK`。
- 框架不分配锁对象；`mutex_create` 返回的指针由框架保存，deinit 时调用
  `mutex_destroy`。
- 每个树节点创建一把锁，锁数量 = 节点数。

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

零堆版本用 `xSemaphoreCreateMutexStatic` 配 `StaticSemaphore_t` 数组。

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

## 裸机/单线程

```c
elib_drv_init_tree(NULL, &tree, nodes, node_capacity);
```

`NULL` port 不创建锁、锁助手直接返回成功；节点锁超时字段被忽略。日志仍可
独立使用。

## 锁规则

- 每个节点一把锁；`read/write/ioctl/open/close` 在节点锁内调用 fops。
- device 转发 controller 时顺序为 `device 锁 → controller 锁`，禁止反向。
- 同一节点 fops 内不得再调该节点的 `elib_drv_*` I/O（非递归锁会自锁）。
- `elib_drv_node_lock()` 只用于驱动内部显式组合多次底层操作；持有后不要再
  调会锁同一节点的框架 I/O。
- 全部 API 只允许任务上下文；ISR 不调用。

## 初始化顺序建议

1. 在调度器启动前或安全启动任务中调用 `elib_drv_init_tree()`。
2. 初始化完成后，各任务再用 `elib_drv_node_find()` 取句柄并做 I/O。
3. 关闭业务后再 `elib_drv_deinit()`；确保所有节点 `open_count == 0`。
