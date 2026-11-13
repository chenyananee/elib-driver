**简体中文** | [English](usage_nodes_io.en.md)

# 统一节点 I/O

[返回 README](../README.md)

## 统一 fops

controller 和 device 使用同一张表：

```c
typedef struct {
    int (*init)(elib_drv_node_t *node);
    int (*deinit)(elib_drv_node_t *node);
    int (*open)(elib_drv_node_t *node, uint32_t flags);
    int (*close)(elib_drv_node_t *node);
    int (*read)(elib_drv_node_t *node, void *buf, size_t len);
    int (*write)(elib_drv_node_t *node, const void *buf, size_t len);
    int (*ioctl)(elib_drv_node_t *node, uint32_t cmd, void *arg);
} elib_drv_fops_t;
```

所有钩子都可为 NULL。驱动私有数据放在节点 `ctx`，用
`elib_drv_node_ctx(node)` 取回。

## 自动 init

`elib_drv_init_tree()` 会自动对每个节点调用一次 `fops->init`（如果存在），
规则：

- controller 先于依赖它的节点；共享 controller 只初始化一次。
- 一次 init 中同一节点只调用一次。
- `deinit` 后再次 `init_tree` 会再次调用 init，所以 fops 必须幂等。
- 失败返回原错误，已成功节点按逆序 deinit。

## open / close

- `open/close` 是**可选**的，只用于调用 fops->open/close、引用计数和
  `ELIB_DRV_DEV_F_EXCLUSIVE`。
- 第一次 open 调用 fops->open；最后一次 close 调用 fops->close。
- 未 open 时 read/write/ioctl 仍然可用，适合“init 已经完成硬件初始化”的驱动。
- 独占节点在已打开时再次 open 返回 `-BUSY`。

## read / write / ioctl

```c
int elib_drv_read(elib_drv_node_t *node, void *buf, size_t len);
int elib_drv_write(elib_drv_node_t *node, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg);
```

- 任意节点都可调用；框架按 `node->dt->fops` 分发。
- read/write 在节点锁内调用 fops；返回值大于 len 映射为
  `-ELIB_DRV_ERR_IO`。
- fops 缺失返回 `-ELIB_DRV_ERR_NOT_SUPPORTED`。
- read/write 的 `buf == NULL && len != 0` 返回 `-INVALID_PARAM`。

## device 转发到 controller

device 驱动按配置的 controller 名找到 controller，再调用同一套 I/O：

```c
static int sensor_read(elib_drv_node_t *node, void *buf, size_t len)
{
    sensor_cfg_t *cfg = (sensor_cfg_t *)elib_drv_node_ctx(node);
    elib_drv_node_t *ctrl =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }

    /* 具体目标/寄存器选择由 controller 驱动约定，可在之前调
     * elib_drv_ioctl(ctrl, ..., cfg) 完成。 */
    return elib_drv_read(ctrl, buf, len);
}
```

- 框架在 device fops 内调用 `elib_drv_read(ctrl, ...)` 时会加 controller
  节点锁。
- 顺序是 `device 锁 → controller 锁`，禁止反向。
- device 不应直接调用 SPI/I2C 底层函数；controller 类型细节由 controller
  fops 负责。
- 如果同一 controller 下多个设备并发且需要“选目标 + 传输”原子完成，
  controller 应提供一个合并的 ioctl，或在 controller fops 内自行保证目标
  切换与传输不可分割；框架只保证单次调用持锁。

## controller 直连

UART、裸 ADC、GPIO 等直接操作 controller 节点：

```c
elib_drv_node_t *uart = elib_drv_node_find("uart0");

(void)elib_drv_open(uart, 0u);
(void)elib_drv_write(uart, "AT\r\n", 4u);
(void)elib_drv_close(uart);
```

与 device 转发共用同一套 API 和同一把 controller 锁。

## 显式加锁

需要把多次 controller 操作合并时，驱动可显式加锁：

```c
if (elib_drv_node_lock(ctrl, 100u) != 0) {
    return -ELIB_DRV_ERR_LOCK;
}
/* 直接调用底层/controller 操作，不能再调 elib_drv_read(ctrl) 等会重复加锁的 API */
(void)elib_drv_node_unlock(ctrl);
```

注意：`elib_drv_read/write/ioctl` 自身会加节点锁，不能在已持有同一节点锁时
调用，否则可能自锁。

## 错误与返回值

| 情况 | 返回 |
|------|------|
| manager 未初始化 | `-NOT_INITIALIZED` |
| node/fops 参数非法 | `-INVALID_PARAM` |
| fops 钩子缺失 | `-NOT_SUPPORTED` |
| 锁超时/失败 | `-LOCK` |
| close 未打开节点 | `-NOT_OPEN` |
| read/write 驱动返回值 > len | `-IO` |
| 节点名/controller 查找失败 | `-NOT_FOUND` / 初始化为 `-INVALID_PARAM` |
