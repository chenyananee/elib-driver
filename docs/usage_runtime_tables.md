**简体中文** | [English](usage_runtime_tables.en.md)

# 运行时注册表

静态表之外，用户还可以给 manager 一块字节缓存作为槽位池，在运行期用 `elib_drv_register_*` 追加 controller/device，用 `elib_drv_unregister_*` 移除并复用槽位。

## 适用场景

- 设备在启动后才被发现（总线扫描、热插拔、配置驱动）；
- 需要按需注册临时设备，用完即注销；
- 希望 RAM 代价完全由自己给的缓存决定，没有 `MAX_*` 宏。

## 槽位池与容量

```text
缓存起始地址 ──► 按指针大小向上对齐（填充丢弃）
可用字节     = bytes - 对齐填充
容量         = 可用字节 / sizeof(槽位)，向下取整
槽位         = 描述符 + 该描述符的 ctx
```

- `elib_drv_runtime_t` 的 `controllers` / `devices` 各给一段 `mem + bytes`；`mem == NULL` 且 `bytes == 0` 表示该类没有运行时槽位。
- 对齐与清零由 `elib_drv_init` 完成；用户不需要关心对齐，也不要在这块内存上放别的东西。
- 容量为 0 时所有 `elib_drv_register_*` 返回 `-ELIB_DRV_ERR_NO_SLOT`。

## 注册流程

1. `elib_drv_register_controller(desc, &out)`：
   - 检查已初始化、`desc`/`name` 合法、静态与运行时表中无重名；
   - 找一个 `ctx.desc == NULL` 的空槽，清零并把描述符**按值拷贝**进去；
   - 覆盖 `desc.ctx` 为槽位内联的 ctx，创建锁（`port != NULL` 时）；
   - 调用 `desc->ops->init`（可以在这里注册子设备）；成功后 `out` 拿到句柄。
2. `elib_drv_register_device(desc, &out)`：
   - 先按名字解析 `desc->controller`（静态表 + 运行时表都查）；
   - 分配空槽、拷贝描述符、建锁，然后给 controller 的 `device_count` 加一；
   - `out` 可为 `NULL`，只注册不取句柄。
3. 注册顺序：controller 必须先于绑定它的 device；device 可以绑定静态或运行时 controller，静态 device 只能绑定静态 controller。

## 注销规则

| 操作 | 条件 | 返回 |
|---|---|---|
| 注销静态项 | 任何情况 | `-ELIB_DRV_ERR_NOT_SUPPORTED` |
| `unregister_controller` | 直连 `open_count != 0` | `-ELIB_DRV_ERR_BUSY` |
| `unregister_controller` | 仍有设备绑定（`device_count != 0`） | `-ELIB_DRV_ERR_BUSY` |
| `unregister_device` | `open_count != 0` | `-ELIB_DRV_ERR_BUSY` |
| `unregister_*` | 句柄不属于运行时槽位 | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `unregister_*` | 重复注销已释放的槽位 | `-ELIB_DRV_ERR_INVALID_PARAM` |
| `unregister_controller` | 在该 controller 的 `ops->init` 内注销自身 | `-ELIB_DRV_ERR_BUSY` |

- 注销 controller 会调用它的 `ops->deinit`；`ops->deinit` 返回错误时槽位**仍会释放**，错误码返回给调用方。
- 注销 device 会让 controller 的 `device_count` 减一，槽位清空后可以被下一次注册复用。
- `desc.name` 必须活到注销为止（查找与日志都读它）；`desc.controller` 只在注册调用期间用于解析。

## 失败清理

| 失败点 | 框架的清理 |
|---|---|
| controller 重名 / 无空槽 / 建锁失败 | 不占槽或清空槽位，`device_count`/`open_count` 不变 |
| controller 的 `ops->init` 失败 | 释放它 `init` 期间注册的所有运行时设备（无论绑定到哪个 controller）、关闭它的直连打开、销毁锁、清空槽位 |
| device 重名 / controller 不存在 / 无空槽 / 建锁失败 | 不占槽，controller 的 `device_count` 不变 |

## 示例

```c
static elib_drv_controller_slot_t rt_ctrls[2];
static elib_drv_device_slot_t     rt_devs[8];

static const elib_drv_runtime_t runtime = {
    .controllers = { rt_ctrls, sizeof rt_ctrls },
    .devices     = { rt_devs,  sizeof rt_devs  },
};

static const elib_drv_config_t cfg = {
    .controllers = NULL, .controller_count = 0,
    .devices     = NULL, .device_count     = 0,
    .runtime     = &runtime,
};

void add_sensor(void)
{
    elib_drv_device_desc_t d = {
        .name = "sensor3", .ops = &sensor_ops, .controller = "i2c1",
    };
    elib_drv_device_t *dev = NULL;

    if (elib_drv_register_device(&d, &dev) == 0) {
        (void)elib_drv_unregister_device(dev);   /* 槽位可复用 */
    }
}
```

## 约束

- 槽位缓存由用户提供，生命周期必须覆盖整个注册期；库不会分配或扩容。
- 运行时表与静态表共享同一个查找命名空间，重名会被注册拒绝。
- `elib_drv_register_*` / `unregister_*` 只在启动或设备发现期调用，不与 I/O 并发。
- 注销后的句柄立即失效：重复注销返回 `-ELIB_DRV_ERR_INVALID_PARAM`；槽位复用后旧句柄不得再用；`find`/`foreach` 不得与注销并发。

## 相关

- [生命周期与初始化](usage_lifecycle.md)
- [静态描述符表](usage_static_tables.md)
- [锁、并发与超时](usage_locking.md)
- [返回 README](../README.md)
