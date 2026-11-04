**简体中文** | [English](usage_device_io.en.md)

# 设备打开与 I/O

device 层是 Linux `file_operations` 风格：`open / close / read / write / ioctl` 五个入口，框架负责参数校验、引用计数、加锁与日志，驱动只实现五个回调。

## 调用时序

```text
elib_drv_open(dev, flags)
  ├─ 未初始化 / 参数非法 → 直接返回
  ├─ 取 device 锁（超时来自 desc.lock_timeout_ms）
  ├─ EXCLUSIVE 且已有打开者 → -BUSY
  ├─ 0 → 1 时调用 ops->open(dev, flags)
  ├─ open_count++，放锁
  └─ 锁外打印 open 日志

read / write / ioctl
  ├─ 取 device 锁
  ├─ open_count == 0 → -NOT_OPEN
  ├─ ops 或对应钩子为 NULL → -NOT_SUPPORTED
  ├─ 在锁内调用 ops->read/write/ioctl
  └─ 放锁；read/write 返回值大于 len 时映射为 -IO
```

## 打开与关闭

- 打开是**引用计数**：同一设备 open 三次，`ops->open` 只调用一次；`open_count` 为 1/2/3。
- `ELIB_DRV_DEV_F_EXCLUSIVE`：已有打开者时再次 open 返回 `-ELIB_DRV_ERR_BUSY`，同一调用者也不例外。
- `close` 先递减计数，只有归零时才调用 `ops->close`；计数为 0 时 close 返回 `-ELIB_DRV_ERR_NOT_OPEN`。
- close 失败语义：计数已经释放，框架返回 `ops->close` 的错误并记录日志，设备按已关闭处理。

| API | 行为 | 失败来源 |
|---|---|---|
| `elib_drv_open` | 计数加一；0→1 时调 `ops->open` | `INVALID_PARAM`、`BUSY`、`LOCK`、驱动错误 |
| `elib_drv_close` | 计数减一；归零时调 `ops->close` | `NOT_OPEN`、`LOCK`、驱动错误 |
| `elib_drv_read` | 在 device 锁内调 `ops->read` | `INVALID_PARAM`、`NOT_OPEN`、`NOT_SUPPORTED`、`LOCK`、`IO` |
| `elib_drv_write` | 在 device 锁内调 `ops->write` | 同 read |
| `elib_drv_ioctl` | 在 device 锁内调 `ops->ioctl` | `INVALID_PARAM`、`NOT_OPEN`、`NOT_SUPPORTED`、`LOCK` |

## I/O 规则

- `read`/`write` 允许返回 0 或小于 `len` 的字节数，表示短读/短写；返回大于 `len` 的值会被框架映射为 `-ELIB_DRV_ERR_IO`。
- `ioctl` 的命令用 `ELIB_DRV_IOC(group, nr)` 生成：group 与 nr 各占 8 位，正数返回值由驱动定义。
- `ops` 在 **device 锁内**被调用：实现里不要再调用同一设备的 `elib_drv_open/close/read/write/ioctl`（非递归锁会自锁）；需要共享总线时按[控制器锁](usage_controller_direct.md)的规则加 controller 锁。
- 设备锁超时取 `device_desc.lock_timeout_ms`，`0` 表示永远等待；拿不到锁返回 `-ELIB_DRV_ERR_LOCK`。

## 参数校验

| 输入 | 检查位置 | 返回 |
|---|---|---|
| `dev == NULL` 或 `dev->desc == NULL` | 所有 device API | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `len > INT_MAX` | `read`/`write` | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `buf == NULL && len > 0` | `read`/`write` | `-ELIB_DRV_ERR_INVALID_PARAM` |
| 未打开 | `read`/`write`/`ioctl` | `-ELIB_DRV_ERR_NOT_OPEN` |
| `ops` 或对应钩子为 `NULL` | `read`/`write`/`ioctl` | `-ELIB_DRV_ERR_NOT_SUPPORTED` |
| 驱动返回 `> len` | `read`/`write` | `-ELIB_DRV_ERR_IO` |

## 示例

```c
/* 驱动侧：五个回调，不需要自己加锁 */
static int eeprom_open(elib_drv_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    return 0;
}

static int eeprom_read(elib_drv_device_t *dev, void *buf, size_t len)
{
    return eeprom_burst_read(elib_drv_device_priv(dev), buf, len);
}

static const elib_drv_ops_t eeprom_ops = {
    eeprom_open, eeprom_close, eeprom_read, eeprom_write, eeprom_ioctl
};

/* 用户侧 */
elib_drv_device_t *dev = elib_drv_device_find("eeprom");
uint8_t buf[16];

if (elib_drv_open(dev, 0) == 0) {
    int n = elib_drv_read(dev, buf, sizeof buf);   /* 0..16 */
    if (n < 0) {
        /* -NOT_OPEN / -NOT_SUPPORTED / -IO / -LOCK */
    }
    (void)elib_drv_close(dev);
}
```

## 常见坑

- 在 `ops->read` 里再次 `elib_drv_read(dev, ...)` 会自锁；要复用逻辑请抽成不带框架入口的内部函数。
- `close` 返回错误并不代表计数没释放；不要因为 close 失败就重复 close。
- `EXCLUSIVE` 只限制并发打开者，不限制同一调用者多次 open：第二次就会返回 BUSY。
- I/O 结果只做上界检查，驱动返回的负数错误码原样透传。

## 相关

- [控制器直连](usage_controller_direct.md)
- [控制器锁与并发](usage_locking.md)
- [生命周期与初始化](usage_lifecycle.md)
- [返回 README](../README.md)
