**简体中文** | [English](usage_controller_direct.en.md)

# 控制器直连

不是所有外设都需要 device 层。UART、裸 ADC、GPIO 这类控制器可以只挂 `fops`，由应用直接 `open / close / read / write / ioctl`，框架同样负责锁、引用计数与日志。

## 适用场景

- 控制器本身就是最终使用对象（串口、ADC 通道、GPIO 组）；
- 不想为每个实例再包一层 device；
- 需要与挂在同一控制器上的 device 共享同一把总线锁。

## fops 表

```c
typedef struct {
    int (*open)(elib_drv_controller_t *ctrl, uint32_t flags);
    int (*close)(elib_drv_controller_t *ctrl);
    int (*read)(elib_drv_controller_t *ctrl, void *buf, size_t len);
    int (*write)(elib_drv_controller_t *ctrl, const void *buf, size_t len);
    int (*ioctl)(elib_drv_controller_t *ctrl, uint32_t cmd, void *arg);
} elib_drv_controller_fops_t;
```

- 五个钩子都可为 `NULL`；整张 `fops` 也可以为 `NULL`。
- `fops == NULL` 或某钩子为 `NULL` 时，open/close 仍然可用（只计数），对应的 I/O 返回 `-ELIB_DRV_ERR_NOT_SUPPORTED`。
- `priv` 通过 `elib_drv_controller_priv(ctrl)` 取回，框架不碰它。

## 调用与锁

```text
elib_drv_controller_open/close/read/write/ioctl(ctrl)
  ├─ 未初始化 / 参数非法 → 直接返回
  ├─ 取 controller 锁（超时来自 controller_desc.lock_timeout_ms）
  ├─ 直连计数未打开时 I/O → -NOT_OPEN
  ├─ 在锁内调用 fops 对应钩子
  ├─ 放锁
  └─ 锁外打印 open/close 日志
```

直连 open/close 与 device 的打开计数**互相独立**：device 打开着不影响直连 open，反之亦然；但两者访问硬件时用的是**同一把 controller 锁**。

## 规则

- 直连打开期间 `elib_drv_deinit` 与 `elib_drv_unregister_controller` 返回 `-ELIB_DRV_ERR_BUSY`。
- `fops` 在 controller 锁内被调用：实现里**不要**再调 `elib_drv_controller_lock`，也**不要**回调 `elib_drv_device_*`（会与 device → controller 的加锁顺序相反）。
- 直连调用只持 controller 锁，不嵌套 device 锁。
- 直连 I/O 的参数校验与 device 一致：`len > INT_MAX`、`buf == NULL && len > 0` 返回 `INVALID_PARAM`；返回大于 `len` 映射 `-IO`。

| 维度 | device 路径 | controller 直连 |
|---|---|---|
| 句柄 | `elib_drv_device_t *` | `elib_drv_controller_t *` |
| 打开计数 | 每设备独立 | 每 controller 独立 |
| 锁 | device 锁；驱动内可再取 controller 锁 | 框架直接取 controller 锁 |
| ops 来源 | `device_desc.ops` | `controller_desc.fops` |
| 注销/拆除 | 设备打开中 BUSY | 直连打开中 BUSY |

## 与 device 混用

- 长期混用是否允许由驱动决定：`fops->open` 里可以用 `elib_drv_controller_device_count(ctrl)` 检查是否已有 device 绑定，必要时返回 `-ELIB_DRV_ERR_BUSY`。
- 直连与 device 的逐次调用不会交错：两者都走同一把 controller 锁。
- 若控制器本身有“独占模式”，应在 `fops->open` 中实现策略，而不是依赖框架。

## 示例

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

static const elib_drv_controller_fops_t uart_fops = {
    uart_open, NULL, uart_read, uart_write, NULL
};

static elib_drv_controller_t uart_ctx;
static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "uart1", .ops = NULL, .fops = &uart_fops,
      .priv = &uart_hw, .ctx = &uart_ctx, .lock_timeout_ms = 100 },
};
```

```c
elib_drv_controller_t *uart = elib_drv_controller_find("uart1");

if (elib_drv_controller_open(uart, 0) == 0) {
    (void)elib_drv_controller_write(uart, "AT\r\n", 4);
    (void)elib_drv_controller_read(uart, buf, sizeof buf);
    (void)elib_drv_controller_close(uart);
}
```

## 常见坑

- 在 `fops->read/write` 里再调 `elib_drv_controller_lock` 会自锁（框架已经持锁）。
- 在 `fops` 里回调 device API 会反转 `device → controller` 的加锁顺序，可能死锁。
- 直连 open 之后忘记 close，会让 `deinit` / `unregister_controller` 一直返回 BUSY。
- `fops == NULL` 时 open/close 仍会成功，只是 I/O 全部 `NOT_SUPPORTED`；不要把它当成“控制器不可用”。

## 相关

- [设备打开与 I/O](usage_device_io.md)
- [锁、并发与超时](usage_locking.md)
- [生命周期与初始化](usage_lifecycle.md)
- [返回 README](../README.md)
