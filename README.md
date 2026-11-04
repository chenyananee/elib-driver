**简体中文** | [English](README.en.md)

# elib-driver

面向 MCU 的轻量驱动框架：**manager / controller / device** 三层，设备操作是
Linux `file_operations` 风格（open / close / read / write / ioctl），静态表
放 flash、运行时上下文只占十几字节，RTOS 互斥通过 port 外置，可选注册一个
printf 风格的流程日志。

- C99，零动态内存；除 manager 单例外没有其它隐藏的全局状态
- 单公共头 `include/elib_drv.h`，库外可完整使用：不改库、不引内部头、无容量宏
- 静态 `const` 描述符表 + 可选运行时槽位表（用户给缓存，容量自动推导）
- 互斥锁与日志都通过接口外置，裸机传 `NULL` 即完全不锁、不打日志

当前版本：**0.2.0**（`ELIB_DRV_VERSION`），单头 `include/elib_drv.h`。

## 特性

- **三层职责明确**：manager 负责校验/建锁/查找/调度；controller 是硬件控制器
  或一类设备的驱动（共享资源 + 总线锁）；device 是挂在 controller 上的实例
  （`file_operations` 实现）
- **两种注册方式**：编译期 `const` 表（描述符在 flash）与运行期槽位表
  （用户缓存，`elib_drv_register_*`），查询与遍历统一
- **controller 可直连**：UART、裸 ADC、GPIO 这类场景可以绕过 device，直接
  `elib_drv_controller_open/read/write/ioctl`；直连与 device 共享同一把
  controller 锁，逐次调用不会交错
- **容量自动推导**：运行时表容量 = 缓存字节数 ÷ 槽位大小，向下取整；静态表容量
  就是表项数，用 `ELIB_DRV_ARRAY_SIZE()` 得到，不存在 `MAX_*` 配置宏
- **打开语义**：引用计数 + 可选独占（`ELIB_DRV_DEV_F_EXCLUSIVE`），句柄就是
  `elib_drv_device_t *`，无文件描述符对象
- **RTOS 无关**：`elib_drv_port_t` 提供互斥量创建/销毁/加锁/解锁，框架自身
  不分配；`port == NULL` 即单线程裸机
- **可选流程日志**：注册 `printf` 或任意 `int (*)(const char *, ...)`，框架在
  **锁外**打印 init / 注册 / 注销 / open / close / 失败等流程；read/write/ioctl
  不打印，支持 `-DELIB_DRV_LOG_ENABLE=0` 整段裁掉

## 用法文档

| 流程 | 文档 |
|------|------|
| 生命周期与初始化 | [docs/usage_lifecycle.md](docs/usage_lifecycle.md) |
| 静态描述符表 | [docs/usage_static_tables.md](docs/usage_static_tables.md) |
| 运行时注册表 | [docs/usage_runtime_tables.md](docs/usage_runtime_tables.md) |
| 设备打开与 I/O | [docs/usage_device_io.md](docs/usage_device_io.md) |
| 控制器直连 | [docs/usage_controller_direct.md](docs/usage_controller_direct.md) |
| 锁、并发与超时 | [docs/usage_locking.md](docs/usage_locking.md) |
| 流程日志 | [docs/usage_logging.md](docs/usage_logging.md) |
| RTOS 移植 | [docs/usage_porting.md](docs/usage_porting.md) |
| 构建与测试 | [docs/usage_build_test.md](docs/usage_build_test.md) |

## 分层与调用路径

```
用户（device 路径）: elib_drv_open/close/read/write/ioctl(dev)
        │
        ▼
manager 门面: 参数/状态校验 → 取设备锁 → 调 ops → 放锁 → (可选)流程日志
        │
        ▼
device ops (file_operations): open/close/read/write/ioctl(dev, ...)
        │            │
        │            └─ 需要共享总线时: elib_drv_controller_lock(ctrl, timeout)
        ▼
controller priv (寄存器/总线/外设) → 硬件

用户（直连路径）: elib_drv_controller_open/close/read/write/ioctl(ctrl)
        │
        ▼
manager 门面: 取 controller 锁 → 调 controller fops → 放锁 → 日志
        │
        ▼
controller priv → 硬件（与 device 路径共用同一把 controller 锁）
```

| 层 | 是什么 | 常量（flash） | 可变（RAM，用户提供） |
|---|---|---|---|
| manager | 注册表 + 调度 + 日志分发 | — | 单例（`initialized`/`port`/表指针/`log`） |
| controller | 硬件控制器/一类设备的驱动与共享资源 | `controller_desc`（name/ops/fops/priv） | 静态：`elib_drv_controller_t`（设备数 + 直连打开数）；运行时：槽位内联 |
| device | 挂在 controller 上的设备实例 | `device_desc`（name/ops/priv/绑定/flags/超时） | 静态：`elib_drv_device_t`；运行时：槽位内联 |

## 内存模型

### 静态表：描述符在 flash，上下文在 RAM

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
    .runtime     = NULL,                     /* 只用静态表 */
};

elib_drv_init(&port, &cfg);                  /* port = NULL 即裸机 */
```

- 描述符只读：库内所有描述符参数都是 `const *`，写描述符的代码直接编译失败。
- 每个表项必须自带 `ctx`；`ctx` 的字段由框架维护，调用方不得写入。
- RAM 成本：32 位下 device 上下文 20 B、controller 16 B（64 位为 32/24 B）。
- `port`、描述符表与运行时缓存必须比 manager 长寿（通常用静态存储）；`config` 结构本身可以放在栈上。

### 运行时表：用户给缓存，容量自动算

```c
static elib_drv_controller_slot_t rt_ctrls[2];
static elib_drv_device_slot_t     rt_devs[8];

static const elib_drv_runtime_t runtime = {
    .controllers = { rt_ctrls, sizeof rt_ctrls },
    .devices     = { rt_devs,  sizeof rt_devs  },
};

/* cfg.runtime = &runtime; 然后 init ... */

elib_drv_device_desc_t d = {
    .name = "sensor3", .ops = &sensor_ops, .controller = "i2c1",
};
elib_drv_device_t *dev = NULL;
elib_drv_register_device(&d, &dev);      /* 运行期追加 */
/* ... */
elib_drv_unregister_device(dev);         /* 运行期移除，槽位可复用 */
```

- 每个槽位 = 同款描述符 + 它的上下文；容量 = 对齐后字节数 ÷ `sizeof(槽位)`，
  向下取整，零头丢弃。
- 缓存起始地址由框架对齐，用户不需要关心对齐。
- 没给缓存（`runtime == NULL` 或容量为 0）时 `elib_drv_register_*` 返回
  `-ELIB_DRV_ERR_NO_SLOT`。
- 运行时注册会拷贝调用方的描述符；`desc.name` 必须活到注销（用于查找），
  `desc.controller` 只在注册调用期间用于按名字解析。
- 静态表项不可注销（返回 `-ELIB_DRV_ERR_NOT_SUPPORTED`）；静态设备只能绑定
  静态 controller，运行时设备可绑定两者。
- 注销后的运行时句柄立即失效：重复注销返回 `-ELIB_DRV_ERR_INVALID_PARAM`，
  槽位被复用后不得再用旧句柄。

### 注册组合矩阵

| controller \ device | 静态 device | 运行时 device |
|---|---|---|
| 静态 controller | 支持：`init` 时按名字绑定 | 支持：`register_device` 时按名字绑定 |
| 运行时 controller | 不支持：`init` 只查静态表，返回 `-ELIB_DRV_ERR_NOT_FOUND` | 支持：先 `register_controller`，再 `register_device` |

- 静态 device 的 `controller` 只在 `elib_drv_init` 时对静态表解析一次，没有延迟绑定/重绑 API；写成 `controller = NULL` 则保持无绑定。
- 运行时 device 可以绑定静态或运行时 controller；静态 device 不能绑定运行时 controller。

## 快速上手

```c
#include "elib_drv.h"
#include <stdio.h>
#include <string.h>

/* ---- 1. 驱动实现：file_operations 风格 ---- */
static int eeprom_open(elib_drv_device_t *dev, uint32_t flags)
{
    (void)flags;
    (void)dev;
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

/* ---- 2. 上下文 + const 描述符表 ---- */
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

/* ---- 3. 启动 ---- */
int app_start(void)
{
    elib_drv_device_t *dev;
    unsigned char buf[16];
    int rc;

    elib_drv_set_log(printf);            /* 可选：直接接 printf */
    rc = elib_drv_init(NULL, &cfg);      /* 裸机；RTOS 见下节 */
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

## 控制器直连（不经过 device）

UART、裸 ADC、GPIO 这类控制器经常不需要 device 层。给 controller 描述符挂上
`fops`，就可以像 device 一样直接 open / close / read / write / ioctl：

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

规则：

- 直连 open/close 是引用计数，与 device 的打开状态**互相独立**；直连打开期间
  `elib_drv_deinit` / `elib_drv_unregister_controller` 返回
  `-ELIB_DRV_ERR_BUSY`。
- 每次直连调用都取 **controller 锁**（超时来自 `desc.lock_timeout_ms`，
  0 = 永远），和 device 驱动访问同一总线时用的是同一把锁，逐次调用不会交错。
- `fops` 在 controller 锁内被调用：实现里**不要**再调
  `elib_drv_controller_lock`，也**不要**回调 `elib_drv_device_*`
  （会与 `device → controller` 的加锁顺序相反）。
- `fops` 为 NULL 或某个钩子为 NULL：open/close 仍可用（只计数），对应 I/O
  返回 `-ELIB_DRV_ERR_NOT_SUPPORTED`。
- 直连与 device 长期混用是否允许由驱动决定：`fops->open` 里可以用
  `elib_drv_controller_device_count(ctrl)` 检查是否已有 device 绑定，必要时
  返回 `-ELIB_DRV_ERR_BUSY`。

## 核心概念

### 返回约定

所有函数返回 `int`：`0` 或正数为成功（`read/write` 是字节数，`ioctl` 由驱动
定义正数语义），负数为失败，值恒为 `-(elib_drv_err_t)`。驱动私有错误从
`-ELIB_DRV_ERR_DRIVER_BASE - n` 起用，框架原样透传。

| 错误码 | 含义 |
|---|---|
| `ELIB_DRV_ERR_INVALID_PARAM` | `NULL` / 空名 / 表项不自洽 / port 回调不全 |
| `ELIB_DRV_ERR_NOT_INITIALIZED` | 未 `elib_drv_init` |
| `ELIB_DRV_ERR_ALREADY_INIT` | 重复 init |
| `ELIB_DRV_ERR_ALREADY_EXIST` | 静态表内或与运行时表重名 |
| `ELIB_DRV_ERR_NOT_FOUND` | 按名字找不到 controller |
| `ELIB_DRV_ERR_NO_SLOT` | 运行时表不存在或已满 |
| `ELIB_DRV_ERR_BUSY` | deinit 时仍有打开的设备或直连打开的 controller，或 init 仍在执行；unregister_controller 时仍有打开的设备/直连打开、仍绑定设备，或在其 init 内注销自身 |
| `ELIB_DRV_ERR_NOT_OPEN` | 未打开就 read/write/ioctl |
| `ELIB_DRV_ERR_NOT_SUPPORTED` | 对应 ops 为 `NULL`；或注销静态项 |
| `ELIB_DRV_ERR_LOCK` | 建锁失败 / 加锁超时 |
| `ELIB_DRV_ERR_TIMEOUT` / `_IO` | 驱动可返回的常规错误 |

### 打开语义

- 打开是引用计数：open 三次，`ops->open` 只调一次；close 到计数归零才调
  `ops->close`。controller 直连的 open/close 遵循同一规则，但计数与 device
  分开。
- close 先递减计数、再在归零时调 `ops->close`；若 `ops->close` 返回负值，
  计数已经释放，框架返回该错误并记录日志（设备仍按已关闭处理）。
- `ELIB_DRV_DEV_F_EXCLUSIVE`：已有打开者时再次 open 返回 `-BUSY`。
- `read/write/ioctl` 要求设备/控制器已打开（`-NOT_OPEN`），并持有对应的锁。
- 设备锁超时取 `desc.lock_timeout_ms`，直连的 controller 锁取
  `controller_desc.lock_timeout_ms`，`0` 表示永远等待；拿不到锁返回
  `-ELIB_DRV_ERR_LOCK`。

### 控制器锁

多个设备共享总线时，由驱动在 ops 内显式加 controller 锁：

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

## 可选流程日志

```c
elib_drv_set_log(printf);                 /* 最简单：直接接 printf */

/* 或者自己的串口/RTT logger */
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

- 框架只在 **释放锁之后** 调用日志回调，回调里不会持有框架锁。
- 回调可能被多个任务并发调用（不同设备同时 open），需自行保证线程安全。
- **不得**在回调里调用任何 `elib_drv_*`（防递归/重入）。
- 记录点：init 开始/成功/失败、controller init/deinit 与失败、注册/注销及失败
  原因、device 的 open/close 及失败、**controller 直连的 open/close 及失败**、
  deinit busy、锁超时。read/write/ioctl 不打印。
- 消息格式固定为 `"[elib_drv] I|W|E: ..."`，只用 `%s/%u/%d`。
- `elib_drv_set_log(NULL)` 关闭；`deinit` 不会清除已注册的回调。
- 编译时 `-DELIB_DRV_LOG_ENABLE=0` 可把全部日志调用与格式串裁掉（默认开启）。

## API

```c
/* manager */
int elib_drv_init(const elib_drv_port_t *port, const elib_drv_config_t *config);
int elib_drv_deinit(void);
elib_drv_controller_t *elib_drv_controller_find(const char *name);
elib_drv_device_t     *elib_drv_device_find(const char *name);
int elib_drv_device_foreach(int (*fn)(elib_drv_device_t *, void *), void *arg);

/* 运行时注册 */
int elib_drv_register_controller(const elib_drv_controller_desc_t *, elib_drv_controller_t **out);
int elib_drv_unregister_controller(elib_drv_controller_t *);
int elib_drv_register_device(const elib_drv_device_desc_t *, elib_drv_device_t **out);
int elib_drv_unregister_device(elib_drv_device_t *);

/* 用户 I/O（device） */
int elib_drv_open(elib_drv_device_t *, uint32_t flags);
int elib_drv_close(elib_drv_device_t *);
int elib_drv_read(elib_drv_device_t *, void *buf, size_t len);
int elib_drv_write(elib_drv_device_t *, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_device_t *, uint32_t cmd, void *arg);

/* 控制器直连（不经过 device） */
int elib_drv_controller_open(elib_drv_controller_t *, uint32_t flags);
int elib_drv_controller_close(elib_drv_controller_t *);
int elib_drv_controller_read(elib_drv_controller_t *, void *buf, size_t len);
int elib_drv_controller_write(elib_drv_controller_t *, const void *buf, size_t len);
int elib_drv_controller_ioctl(elib_drv_controller_t *, uint32_t cmd, void *arg);

/* 驱动侧 */
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

/* 流程日志 */
typedef int (*elib_drv_log_fn)(const char *fmt, ...);
int elib_drv_set_log(elib_drv_log_fn fn);
```

`ioctl` 命令建议用 `ELIB_DRV_IOC(group, nr)` 生成，驱动私有命令定义在驱动
自己的头文件里，不进库。

## RTOS 移植

唯一需要移植的是 `elib_drv_port_t`：

```c
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;
```

四个回调必须齐全；`mutex_lock` 成功返回 0、失败返回负值（框架统一映射为
`-ELIB_DRV_ERR_LOCK`）；`timeout_ms` 为 `ELIB_DRV_WAIT_FOREVER` 或毫秒。
**框架自身不做任何分配**，互斥量存储由 port 负责（堆或静态池都可以）。

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

零堆版本把 `xSemaphoreCreateMutex` 换成 `xSemaphoreCreateMutexStatic`
（配 `StaticSemaphore_t` 数组）。

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

### 裸机 / 单线程

`elib_drv_init(NULL, &cfg)`：不建锁、不加锁、零额外 RAM，日志仍可独立使用。

## 并发与锁规则

| 锁 | 保护对象 | 谁取 | 超时 |
|---|---|---|---|
| device 锁 | 打开计数 + 设备状态 + ops 串行化 | 框架 | `device_desc.lock_timeout_ms`（0 = 永远） |
| controller 锁 | 总线/共享寄存器 + 直连 fops 串行化 | 框架（直连 API）+ 驱动（device ops 内共享总线时） | 直连：`controller_desc.lock_timeout_ms`；驱动自选 |

- 允许 `device → controller` 嵌套；禁止反向。直连调用只持 controller 锁，
  不嵌套 device 锁。同一设备/控制器的 ops 或 fops 内禁止再调对应的
  `elib_drv_*read/write/ioctl/open/close`（非递归锁会自锁）。
- `init` / `deinit` / `register_*` / `unregister_*` 只在启动或设备发现期调用，
  不与 I/O 并发；`find` / `foreach` 只读，可与 I/O 并发，但不得与
  `register_*` / `unregister_*` 并发（会拿到悬空指针）；`foreach` 回调内
  不得注册或注销。
- 全部 API 仅任务上下文可用，ISR 不调用；驱动需要 ISR 访问时用自己的机制。
- 注销静态项返回 `-NOT_SUPPORTED`；运行时项在设备打开或 controller 直连打开
  期间返回 `-BUSY`。
- 日志在锁外打印，回调不得回调 `elib_drv_*`，需线程安全。

## 构建与测试

无构建系统：把 `include/` 加进头文件路径，把 `src/*.c` 加进工程即可。

```bash
# 编译库
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_device.c

# 功能回归（只用公共头）
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv && ./t_drv

# 并发验证（POSIX，不属于默认构建）
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt && ./t_mt

# 日志裁剪构建
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c
```

## 目录

```
include/elib_drv.h       唯一公共头：错误码/描述符/上下文/port/API/日志
src/elib_drv_internal.h  内部单例、锁助手、日志宏
src/elib_drv_manager.c   init/deinit/校验/双表/注册注销/查找/遍历
src/elib_drv_device.c    open/close/read/write/ioctl/访问器/控制器锁
test/test_elib_drv.c     功能 + 双表 + 日志 + 锁纪律回归
test/test_elib_drv_mt.c  POSIX 多线程验证（-pthread）
docs/usage_*.md          9 篇流程文档（每篇配 .en.md 英文版）
```

## License

MIT License
