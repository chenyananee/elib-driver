**简体中文** | [English](usage_locking.en.md)

# 锁、并发与超时

框架只使用 port 提供的互斥量：device 路径取 device 锁，直连路径取 controller 锁，驱动在 device ops 里按需再取 controller 锁。`port == NULL` 时完全不建锁、不加锁。

## port 契约

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

- 四个回调必须齐全，否则 `elib_drv_init` 返回 `-ELIB_DRV_ERR_INVALID_PARAM`。
- `mutex_create` 返回 `NULL` 视为建锁失败：`init` 回滚并返回 `-ELIB_DRV_ERR_LOCK`。
- `mutex_lock` 成功返回 0，超时/失败返回负值；框架统一映射为 `-ELIB_DRV_ERR_LOCK`。
- 互斥量的存储由 port 负责（堆或静态池），框架自身不分配。

## 锁的覆盖范围

| 锁 | 保护对象 | 谁取 |
|---|---|---|
| device 锁 | 打开计数、设备状态、`ops` 串行化 | 框架（device API） |
| controller 锁 | 总线/共享寄存器、直连 `fops` 串行化 | 框架（直连 API）；驱动在 device ops 内按需 |

- 设备锁只在 `elib_drv_open/close/read/write/ioctl` 期间持有。
- controller 锁在直连 `open/close/read/write/ioctl` 期间由框架持有；device 驱动访问共享总线时用 `elib_drv_controller_lock` 自己加。
- 日志回调在锁外调用，回调内不得再调用任何 `elib_drv_*`。

## 超时

| 配置 | 行为 |
|---|---|
| `lock_timeout_ms == 0` | 映射为 `ELIB_DRV_WAIT_FOREVER`（`UINT32_MAX`），一直等待 |
| `lock_timeout_ms > 0` | 等待指定毫秒；超时返回 `-ELIB_DRV_ERR_LOCK` |
| 直连路径 | 使用 `controller_desc.lock_timeout_ms` |
| device 路径 | 使用 `device_desc.lock_timeout_ms`；驱动自取的 controller 锁由驱动自选超时 |

## 加锁顺序

- 允许 `device 锁 → controller 锁`；**禁止反向**。
- 直连调用只持 controller 锁，不嵌套 device 锁。
- 同一设备/控制器的 `ops`/`fops` 内禁止再调对应的 `elib_drv_*open/close/read/write/ioctl`（非递归锁会自锁）。

## 并发矩阵

| API 组 | 与 I/O 并发 | 与注册/注销并发 | 说明 |
|---|---|---|---|
| `elib_drv_init` / `elib_drv_deinit` | 否 | 否 | 启动/关机期调用 |
| `elib_drv_register_*` / `unregister_*` | 否 | 否 | 设备发现期调用 |
| `elib_drv_controller_find` / `elib_drv_device_find` / `elib_drv_device_foreach` | 是 | 否 | 只读；注销会留下悬空指针 |
| `elib_drv_open` / `close` / `read` / `write` / `ioctl` | 是 | 否 | 由 device 锁串行化 |
| 控制器直连 API | 是 | 否 | 由 controller 锁串行化 |
| `elib_drv_controller_lock` / `unlock` | 是 | 否 | 驱动内部使用，注意顺序 |

## find / foreach

- `find` / `foreach` 不加锁，返回的是库内 ctx 指针；可与 I/O 并发。
- **不得**与 `register_*` / `unregister_*` 并发：注销后指针立即失效。
- `foreach` 回调内不得注册/注销设备，也不得销毁 manager；回调返回非 0 时立即停止遍历并原样返回该值。
- `foreach` 的遍历顺序固定：先静态表，再运行时槽位（槽位按索引顺序）。

## ISR 与重入

- 全部 API 仅任务上下文可用；ISR 需要访问设备时用自己的机制（标志/队列 + 主循环）。
- 驱动 `ops`/`fops` 内不能再次进入同一设备/控制器的框架入口。
- 日志回调可能在多个任务中并发执行，必须自行保证线程安全，且不得回调 `elib_drv_*`。

## 示例

```c
/* 驱动在 device ops 内共享总线：按 device → controller 顺序加锁 */
static int sensor_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    elib_drv_controller_t *bus = elib_drv_device_controller(dev);
    int rc = elib_drv_controller_lock(bus, ELIB_DRV_WAIT_FOREVER);

    if (rc != 0) {
        return rc;                       /* -ELIB_DRV_ERR_LOCK */
    }
    rc = i2c_burst_read(elib_drv_controller_priv(bus), dev, buf, len);
    (void)elib_drv_controller_unlock(bus);
    return rc;
}
```

## 相关

- [控制器直连](usage_controller_direct.md)
- [设备打开与 I/O](usage_device_io.md)
- [RTOS 移植](usage_porting.md)
- [返回 README](../README.md)
