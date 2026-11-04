**简体中文** | [English](usage_lifecycle.en.md)

# 生命周期与初始化

manager 是单例：`elib_drv_init` 一次性完成 port、静态表与运行时槽位池的接线，`elib_drv_deinit` 按固定顺序拆除。本页描述这两条主流程、每个失败点由谁回滚，以及 deinit 的 BUSY 条件。

## 适用场景

- 系统启动时初始化驱动框架，注册静态 controller/device；
- 需要了解 `elib_drv_init` 失败后硬件与内存处于什么状态；
- 需要在关机/重启前安全地 `elib_drv_deinit`。

## 流程图

```text
elib_drv_init(port, cfg)
  ├─ 已初始化？ ── 是 → -ELIB_DRV_ERR_ALREADY_INIT
  ├─ port 校验（NULL，或四个回调齐全）
  ├─ config 校验（表指针、名字、ctx、重名、controller 引用）
  ├─ 清空 manager 单例（保留 log 回调）
  ├─ 对齐并清零运行时槽位池，推导容量
  ├─ 写静态 controller/device ctx，按名字绑定 controller
  ├─ port != NULL 时逐项创建锁
  ├─ initialized = 1
  ├─ 逐个 controller 调 ops->init（可注册自己的设备）
  └─ 任一步失败 → 回滚并返回该错误
```

## 初始化步骤

1. **幂等检查**：`elib_drv_g_manager.initialized` 为 1 时直接返回 `-ELIB_DRV_ERR_ALREADY_INIT`，不改任何状态。
2. **port 校验**：`NULL` 表示裸机；非 `NULL` 时 `mutex_create/destroy/lock/unlock` 四个回调必须齐全，否则 `-INVALID_PARAM`。
3. **config 校验**：见下方错误表。所有检查都在改动 manager 之前完成，失败时框架状态不变。
4. **接线静态表**：每个 controller/device 的 `ctx` 被清零，写入 `desc` 指针与初始计数；device 的 `controller` 名字只在静态 controller 表里解析。
5. **建锁**：`port != NULL` 时先 controller 后 device，锁名就是描述符 `name`；任一把创建失败即回滚。
6. **controller init**：`initialized` 已置位，所以 `ops->init` 内可以调用 `elib_drv_register_device` 等 API；`ops->init` 返回负值即失败。

## 失败回滚

| 失败点 | 框架做的清理 | 调用方看到 |
|---|---|---|
| port 校验 | 不改 manager | `-ELIB_DRV_ERR_INVALID_PARAM` |
| config 校验 | 不改 manager | `INVALID_PARAM` / `ALREADY_EXIST` / `NOT_FOUND` |
| 创建锁 | 已创建的锁全部销毁，静态 ctx 清零，不调用任何 `ops` | `-ELIB_DRV_ERR_LOCK` |
| controller `ops->init` | 释放 init 期间注册的所有运行时设备（无论绑定到哪个 controller）、关闭失败 controller 的直连、销毁锁；**已成功**的 controller 会调用 `deinit`，**失败的这个不会** | 该 `ops->init` 的返回值 |
| 成功 | 无 | `0` |

> 失败的 `ops->init` 仍必须清理硬件状态与其他 controller 上的直连；框架会释放它 init 期间注册的所有运行时设备、关闭失败 controller 的直连并销毁锁（静态 init 失败时，其余未初始化 controller 的直连也会统一关闭）。

## deinit 流程

`elib_drv_deinit` 先确认自己不在 `init` 过程中，再做四项 BUSY 检查，全部通过后进入 teardown：

| 检查项 | 条件 |
|---|---|
| init 进行中 | `initializing` 非 0，直接返回 `-ELIB_DRV_ERR_BUSY` |
| 静态 device | `open_count != 0` |
| 运行时 device 槽 | `desc != NULL && open_count != 0` |
| 静态 controller | `open_count != 0`（直连） |
| 运行时 controller 槽 | `desc != NULL && open_count != 0`（直连） |
| controller 注销自身 | 在该 controller 的 `ops->init` 内调用 `elib_drv_unregister_controller` 返回 `-BUSY` |

teardown 顺序固定，且按注册/数组顺序**逆序**释放：

1. 逆序释放静态 device（必要时调用 `ops->close`）；
2. 逆序释放运行时 device 槽；
3. 逆序释放运行时 controller 槽（关闭直连、调用 `ops->deinit`、销毁锁）；
4. 逆序释放静态 controller（已成功 init 的调用 `deinit`；未成功的先关闭直连再销毁锁）；
5. `memset` manager，但**保留 log 回调**，返回第一个 `ops->close` / `ops->deinit` 错误。

deinit 会把能拆的全部拆完：即使某个 `ops->deinit` 返回错误，后续 controller 仍会继续释放，调用方拿到的是第一个错误码。

## 错误码

| 错误码 | 触发条件 |
|---|---|
| `ELIB_DRV_ERR_ALREADY_INIT` | 重复 `elib_drv_init` |
| `ELIB_DRV_ERR_INVALID_PARAM` | `cfg == NULL`；表指针空但计数非 0；空名；`ctx == NULL`；port 回调不全 |
| `ELIB_DRV_ERR_ALREADY_EXIST` | controller 或 device 表内重名 |
| `ELIB_DRV_ERR_NOT_FOUND` | device 的 `controller` 名字找不到 |
| `ELIB_DRV_ERR_LOCK` | `mutex_create` 返回 `NULL` |
| `ELIB_DRV_ERR_NOT_INITIALIZED` | 未 init 就调用其它 API 或重复 deinit |
| `ELIB_DRV_ERR_BUSY` | deinit 时仍有打开的设备/直连打开的 controller，或 `init` 仍在执行；controller 在自身 `init` 内注销自己 |
| controller `ops->init` 的错误 | 原样透传（负值） |

## 示例

```c
#include "elib_drv.h"

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

int app_boot(void)
{
    int rc = elib_drv_init(NULL, &cfg);   /* NULL port = 裸机 */

    if (rc != 0) {
        return rc;                        /* 框架已回滚，无需再清理 */
    }
    elib_drv_device_t *dev = elib_drv_device_find("eeprom");
    if (dev == NULL) {
        (void)elib_drv_deinit();
        return -1;
    }
    return 0;
}

void app_shutdown(void)
{
    /* 必须先关闭所有设备与直连 controller，否则返回 -BUSY */
    (void)elib_drv_deinit();
}
```

## 注意事项

- `init` 失败后可以重试；`deinit` 之后也可以重新 `init`（manager 已清零）。
- `elib_drv_set_log` 可在 `init` 之前调用，`deinit` 不会清除回调。
- controller 的 `ops->init` 内可以注册运行时设备，但不要在 `ops->init` 里调用 `elib_drv_deinit`。
- deinit 的 BUSY 检查是无锁的，前提是 `init/deinit` 不与 I/O 并发（见[锁与并发](usage_locking.md)）。

## 相关

- [静态描述符表](usage_static_tables.md)
- [运行时注册表](usage_runtime_tables.md)
- [锁、并发与超时](usage_locking.md)
- [返回 README](../README.md)
