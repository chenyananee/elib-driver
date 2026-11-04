**简体中文** | [English](usage_static_tables.en.md)

# 静态描述符表

编译期把 controller/device 描述符写进 flash，运行时上下文（`ctx`）由用户在 RAM 里各声明一份——这是零动态内存的主路径。

## 适用场景

- 设备集合在编译期就固定（板载 EEPROM、传感器、UART 等）；
- 不想为注册表付出 RAM 代价，也不需要在运行期增删设备；
- 希望描述符只读、被链接器放进 flash。

## 数据模型

| 对象 | 存放位置 | 谁写 | 内容 |
|---|---|---|---|
| `controller_desc` | flash（`const`） | 用户 | `name`、`ops`、`fops`、`priv`、`lock_timeout_ms` |
| `elib_drv_controller_t` | RAM（用户声明） | 框架 | `desc`、`lock`、`device_count`、`open_count` |
| `device_desc` | flash（`const`） | 用户 | `name`、`ops`、`priv`、`ctx`、`controller`、`flags`、`lock_timeout_ms` |
| `elib_drv_device_t` | RAM（用户声明） | 框架 | `desc`、`lock`、`controller`、`open_count` |

`elib_drv_config_t` 只保存指向两张表的指针和表项数；库不会复制静态描述符。

## 示例

```c
#include "elib_drv.h"

static elib_drv_controller_t i2c1_ctx;
static elib_drv_device_t     eeprom_ctx;

static const elib_drv_controller_desc_t ctrls[] = {
    { .name = "i2c1", .ops = NULL, .priv = &i2c1_hw, .ctx = &i2c1_ctx },
};
static const elib_drv_device_desc_t devs[] = {
    { .name = "eeprom", .ops = &eeprom_ops, .priv = &eeprom_info,
      .ctx = &eeprom_ctx, .controller = "i2c1",
      .lock_timeout_ms = 100 },
};

static const elib_drv_config_t cfg = {
    .controllers = ctrls, .controller_count = ELIB_DRV_ARRAY_SIZE(ctrls),
    .devices     = devs,  .device_count     = ELIB_DRV_ARRAY_SIZE(devs),
    .runtime     = NULL,
};
```

## 绑定与校验

- device 的 `controller` 是**名字**，`init` 时只在静态 controller 表里解析一次；绑定结果写进 device 的 `ctx.controller`。
- 每个表项必须自带 `ctx`；`ctx` 的字段由框架维护，调用方不得写入。
- 名字必须非空且在同一张表内唯一；controller 与 device 是两个独立命名空间，可以同名。
- 校验失败时 `elib_drv_init` 直接返回，不创建锁、不调用任何 `ops`。

| `elib_drv_init` 返回 | 场景 |
|---|---|
| `-ELIB_DRV_ERR_INVALID_PARAM` | 空名、`ctx == NULL`、表指针为空但计数非 0 |
| `-ELIB_DRV_ERR_ALREADY_EXIST` | 同一张表内重名 |
| `-ELIB_DRV_ERR_NOT_FOUND` | device 的 `controller` 名字不存在 |

## 内存成本

- 32 位目标：`elib_drv_controller_t` 12 B，`elib_drv_device_t` 16 B（不含锁本身，锁由 port 分配）。
- 64 位目标：`elib_drv_controller_t` 24 B，`elib_drv_device_t` 32 B。
- 描述符只占 flash；RAM 里只有每个表项一份 `ctx`。

## 约束

- 所有描述符参数在 API 中都是 `const *`，写描述符的代码无法通过编译。
- 静态表项不可注销：`elib_drv_unregister_*` 返回 `-ELIB_DRV_ERR_NOT_SUPPORTED`。
- 静态 device 只能绑定静态 controller；运行期注册的 device 才能绑定运行时 controller。
- `name` 与 `priv` 的生命周期必须覆盖整个使用期；库只保存指针。

## 相关

- [生命周期与初始化](usage_lifecycle.md)
- [运行时注册表](usage_runtime_tables.md)
- [设备打开与 I/O](usage_device_io.md)
- [返回 README](../README.md)
