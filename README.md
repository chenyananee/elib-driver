**简体中文** | [English](README.en.md)

# elib-driver

面向 MCU 的设备树驱动框架：一棵静态设备树描述全部 controller/device 节点，
节点名唯一，每个节点一份统一 `fops`、一个 `void *ctx`。`elib_drv_init_tree()`
自动按 controller 先于依赖节点的顺序初始化全部节点；设备 I/O 和 controller
直连共用同一套 `open/close/read/write/ioctl`。

- C99，零动态内存；库本身不 `malloc/free`，树、句柄数组、ctx、锁都由用户提供
- 单公共头 `include/elib_drv.h`，无内部头依赖，无容量宏
- 统一节点：不再区分 controller/device 类型；有 children 的节点可作 controller，
  有 controller/父关系的节点是 device
- 统一 fops：`init/deinit/open/close/read/write/ioctl` 一套表
- 统一 I/O：`elib_drv_read/write/ioctl(node, ...)` 可作用于任意节点；
  device 驱动按 controller 名找到 controller 后直接调用同一套 API
- RTOS 无关：`elib_drv_port_t` 提供互斥量创建/销毁/加锁/解锁；`NULL` port 即裸机
- 可选 printf 风格流程日志，锁外打印，支持整段裁剪和行尾配置

当前版本：**2.0.0**（`ELIB_DRV_VERSION`）。

## 特性

- **设备树是唯一配置来源**：节点的 `name`、`fops`、`ctx`、`controller`、
  `flags`、锁超时、children 都在树中；没有 descriptor/static/runtime 注册表
- **节点名唯一**：`elib_drv_node_find()`、`elib_drv_of_find()`、device 找
  controller 都用节点名
- **自动 controller-first 初始化**：`init_tree` 递归先初始化 controller，再初始化
  依赖它的节点；同一节点一次 init 只调用一次 `fops->init`；失败按逆序回滚
- **fops 幂等**：`init/deinit` 可能在 deinit→init 循环中再次调用；用户实现需可重复
- **统一 I/O**：`open/close` 仅调用可选 `fops->open/close` 并维护引用计数/独占；
  `read/write/ioctl` 在节点初始化后即可用，不强制先 open
- **controller 直连**：UART、裸 ADC、GPIO 等直接对 controller 节点调
  `elib_drv_read/write/ioctl`
- **device 转发**：device fops 内用
  `elib_drv_node_find(elib_drv_node_controller_name(node))` 找到 controller，
  再调同一套 I/O；框架负责 controller 节点锁
- **of 风格查询**：`elib_drv_of_find/of_config/of_name/of_controller/of_children`
- **离线检查脚本**：`scripts/dt_check.py` 把零散 children 数组展开成树状结构，
  检查重名、controller 引用、深度和句柄容量，无需编译
- **零动态内存**：节点句柄数组由用户静态提供，或用
  `elib_drv_dt_node_count()` 先统计后自行分配

## 快速上手

```c
#include "elib_drv.h"

/* ---- 1. 节点 ctx：硬件配置/状态，一个 void* ---- */
typedef struct { uint32_t base; uint32_t baud; } uart_cfg_t;
typedef struct { uint32_t addr; } sensor_cfg_t;

static uart_cfg_t   uart0_cfg  = { 0x40011000u, 115200u };
static sensor_cfg_t sensor_cfg = { 0x20u };

/* ---- 2. 统一 fops ---- */
static int uart_init(elib_drv_node_t *node)
{
    uart_cfg_t *cfg = (uart_cfg_t *)elib_drv_node_ctx(node);
    return uart_hw_init(cfg->base, cfg->baud);
}

static int uart_read(elib_drv_node_t *node, void *buf, size_t len)
{
    (void)node;
    return uart_hw_read(buf, len);
}

static int uart_write(elib_drv_node_t *node, const void *buf, size_t len)
{
    (void)node;
    return uart_hw_write(buf, len);
}

static int sensor_read(elib_drv_node_t *node, void *buf, size_t len)
{
    sensor_cfg_t *cfg = (sensor_cfg_t *)elib_drv_node_ctx(node);
    elib_drv_node_t *ctrl =
        elib_drv_node_find(elib_drv_node_controller_name(node));

    (void)cfg;                 /* 地址/寄存器选择由 controller 驱动约定 */
    if (ctrl == NULL) {
        return -ELIB_DRV_ERR_NOT_FOUND;
    }
    return elib_drv_read(ctrl, buf, len);
}

static const elib_drv_fops_t uart_fops = {
    .init = uart_init, .read = uart_read, .write = uart_write,
};
static const elib_drv_fops_t sensor_fops = {
    .read = sensor_read,
};

/* ---- 3. 设备树 ---- */
static const elib_drv_dt_node_t uart_children[] = {
    { .name = "rhsensor", .fops = &sensor_fops, .ctx = &sensor_cfg,
      .controller = "uart0" },
};

static const elib_drv_dt_node_t roots[] = {
    { .name = "uart0", .fops = &uart_fops, .ctx = &uart0_cfg,
      .children = uart_children,
      .child_count = ELIB_DRV_ARRAY_SIZE(uart_children),
      .lock_timeout_ms = 100u },
};

static const elib_drv_dt_t tree = {
    .children = roots, .child_count = ELIB_DRV_ARRAY_SIZE(roots),
};

/* ---- 4. 初始化：句柄数量可以先统计 ---- */
static elib_drv_node_t g_nodes[2];

int app_boot(void)
{
    unsigned node_count = 0u;
    elib_drv_node_t *sensor;
    elib_drv_node_t *uart;
    uint8_t buf[16];
    int rc;

    elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);

    rc = elib_drv_dt_node_count(&tree, &node_count);
    if (rc != 0 || node_count > ELIB_DRV_ARRAY_SIZE(g_nodes)) {
        return -1;
    }
    rc = elib_drv_init_tree(NULL, &tree, g_nodes,
                            ELIB_DRV_ARRAY_SIZE(g_nodes));
    if (rc != 0) {
        return rc;
    }

    /* controller 直连 */
    uart = elib_drv_node_find("uart0");
    (void)elib_drv_open(uart, 0u);
    (void)elib_drv_write(uart, "boot\r\n", 6u);
    (void)elib_drv_close(uart);

    /* device：内部找 controller 并转发 */
    sensor = elib_drv_node_find("rhsensor");
    (void)elib_drv_read(sensor, buf, sizeof buf);

    (void)elib_drv_deinit();
    return 0;
}
```

运行时申请句柄（仍由用户负责释放）：

```c
unsigned n = 0u;
elib_drv_node_t *nodes;

if (elib_drv_dt_node_count(&tree, &n) != 0) {
    return -1;
}
nodes = (n != 0u) ? malloc(n * sizeof(*nodes)) : NULL;
if (n != 0u && nodes == NULL) {
    return -1;
}
if (elib_drv_init_tree(NULL, &tree, nodes, n) != 0) {
    free(nodes);
    return -1;
}
/* ... */
elib_drv_deinit();
free(nodes);
```

## 设备树检查脚本

不编译、不运行固件，直接解析 C 源里的 `elib_drv_dt_node_t` 数组：

```bash
python scripts/dt_check.py src/app_dt.c --root roots
```

直接打印树状结果：

```text
roots
+- uart0 fops=&uart_fops ctx=&uart0_cfg lock_timeout_ms=100u child_count=ELIB_DRV_ARRAY_SIZE(uart_children)
|  `- console fops=&console_fops ctx=&console_cfg controller=uart0 lock_timeout_ms=100u
+- spi1 fops=&spi_fops ctx=&spi1_cfg lock_timeout_ms=100u child_count=ELIB_DRV_ARRAY_SIZE(spi_children)
|  +- rhsensor fops=&sensor_fops ctx=&sensor_cfg controller=spi1 lock_timeout_ms=0u
|  `- flash fops=&flash_fops ctx=&flash_cfg controller=spi1 flags=ELIB_DRV_DEV_F_EXCLUSIVE lock_timeout_ms=50u
`- i2c1 fops=&i2c_fops ctx=&i2c1_cfg lock_timeout_ms=100u child_count=ELIB_DRV_ARRAY_SIZE(i2c_children)
   +- eeprom fops=&eeprom_fops ctx=&eeprom_cfg controller=i2c1 flags=ELIB_DRV_DEV_F_EXCLUSIVE lock_timeout_ms=100u
   `- tmp102 fops=&tmp102_fops ctx=&tmp102_cfg controller=i2c1 lock_timeout_ms=50u
```

JSON 使用嵌套 `children`，节点里只有设备树静态字段。下面是 i2c1 挂载
`eeprom`、`tmp102` 两个设备的片段：

```json
[
  {
    "name": "i2c1",
    "fops": "&i2c_fops",
    "ctx": "&i2c1_cfg",
    "controller": null,
    "flags": "0",
    "lock_timeout_ms": "100u",
    "child_count": "ELIB_DRV_ARRAY_SIZE(i2c_children)",
    "children": [
      {
        "name": "eeprom",
        "fops": "&eeprom_fops",
        "ctx": "&eeprom_cfg",
        "controller": "i2c1",
        "flags": "ELIB_DRV_DEV_F_EXCLUSIVE",
        "lock_timeout_ms": "100u",
        "child_count": "0",
        "children": []
      },
      {
        "name": "tmp102",
        "fops": "&tmp102_fops",
        "ctx": "&tmp102_cfg",
        "controller": "i2c1",
        "flags": "0",
        "lock_timeout_ms": "50u",
        "child_count": "0",
        "children": []
      }
    ]
  }
]
```

输出只包含 `name`、`fops`、`ctx`、`controller`、`flags`、`lock_timeout_ms`、
`child_count` 和嵌套 `children`。诊断摘要、错误和警告写到 stderr，因此
`> report.json` 得到的文件只有树本身。脚本检查重名、缺失 controller、
controller 环、children 数组未声明、`child_count` 不一致、深度超限和句柄容量。

```bash
# 自动选择根数组
python scripts/dt_check.py src/app_dt.c

# JSON/CSV，便于 CI 或表格工具
python scripts/dt_check.py src/app_dt.c --root roots --format json
python scripts/dt_check.py src/app_dt.c --root roots --format csv

# 检查句柄容量，警告也失败
python scripts/dt_check.py src/app_dt.c --root roots --capacity 8 --strict
```

详见 [docs/usage_device_tree.md](docs/usage_device_tree.md#离线检查脚本)。

## 架构与调用路径

```text
elib_drv_init_tree()
  → 校验树/节点名/深度/controller 引用
  → 统计节点数，检查用户句柄数组容量
  → 预序填充句柄、建每节点锁
  → 递归 init_node(controller) → fops->init(node)
  → 失败按 init_order 逆序回滚

用户/device I/O:
  elib_drv_read/write/ioctl(node)
    → 取节点锁
    → 分发 node->dt->fops
    → 放锁

device 转发:
  device fops 内
    → elib_drv_node_controller_name(node)
    → elib_drv_node_find(name)
    → elib_drv_read/write/ioctl(controller_node)
      → 取 controller 节点锁，分发 controller fops
```

## 核心 API

| API | 说明 |
|-----|------|
| `elib_drv_dt_node_count(tree, &n)` | 统计整棵树节点数，可用于申请句柄数组 |
| `elib_drv_init_tree(port, tree, nodes, cap)` | 校验、建锁、自动初始化全部节点 |
| `elib_drv_deinit()` | 检查打开计数，逆序 deinit，销毁锁 |
| `elib_drv_node_find(name)` | 按节点名取运行时句柄 |
| `elib_drv_node_foreach(fn, arg)` | 遍历所有节点 |
| `elib_drv_open/close(node, ...)` | 可选 open/close，引用计数 + 独占 |
| `elib_drv_read/write/ioctl(node, ...)` | 统一节点 I/O |
| `elib_drv_node_lock/unlock` | 驱动组合多次 controller 操作时显式加锁 |
| `elib_drv_node_name/ctx/controller/...` | 句柄访问器 |
| `elib_drv_of_find/config/name/controller/children` | 设备树常查 API |

错误码见 `elib_drv_err_t`。`read/write` 返回字节数；驱动返回值大于 `len`
会被映射为 `-ELIB_DRV_ERR_IO`。

## 初始化与生命周期

- `elib_drv_dt_node_count()`：只校验 children 指针/数量、深度（≤ 8）和计数，
  不检查重名/controller 引用；空树返回 0。
- `elib_drv_init_tree()`：要求未初始化；节点总数 ≤ `node_capacity`。
  每个节点自动调用一次 `fops->init`（若有）；controller 先于依赖节点。
  初始化失败返回原错误；已成功节点按逆序 `deinit`，manager 复位。
- `elib_drv_deinit()`：任一节点 `open_count != 0` 返回 `-BUSY`；按 init 逆序
  调用 `deinit`，销毁锁；日志配置保留；可再次 init。
- `fops->init/deinit/open/close` 需幂等：deinit 后再次 init_tree 会再次调用
  init；open/close 可能多轮进入。

## 锁、并发与 controller 转发

- 每个节点一把锁，超时取节点 `lock_timeout_ms`；0 表示 `WAIT_FOREVER`。
- `read/write/ioctl` 在节点锁内调用 fops；`open/close` 也在节点锁内。
- device 转发调用 controller I/O 时顺序为 `device 节点锁 → controller 节点锁`；
  禁止反向，也禁止在同一节点 fops 内再次调用该节点的 I/O。
- controller fops 被 device 转发时已持有 controller 锁，不得再次锁同一节点。
- `elib_drv_node_lock/unlock` 用于需要把多次 controller 操作合并的驱动。

## 流程日志

```c
elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);
```

- 0 = 不追加行尾，1 = `\n`（推荐），2 = `\r\n`；非法值返回
  `-ELIB_DRV_ERR_INVALID_PARAM`。
- 回调在框架锁外调用，必须线程安全，且不得回调 `elib_drv_*`。
- `-DELIB_DRV_LOG_ENABLE=0` 整段裁掉日志；`set_log` API 仍存在。
- 日志覆盖 init/deinit/open/close/失败/锁超时；read/write/ioctl 不打印。

## RTOS 移植

`elib_drv_port_t` 四个回调：`mutex_create/destroy/lock/unlock`。
`NULL` port 表示裸机不锁。完整示例见
[docs/usage_porting.md](docs/usage_porting.md)。

## 构建与测试

无构建系统：把 `include/` 加进头文件路径，把 `src/*.c` 加进工程。

```bash
# 编译库
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_tree.c src/elib_drv_node.c

# 功能回归（只用公共头）
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv && ./t_drv

# 日志裁剪构建
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c

# 并发验证（POSIX pthread，不属于默认构建）
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt && ./t_mt
```

测试覆盖：树计数/深度/校验、auto controller-first init、失败回滚、幂等/reinit、
统一 I/O、缺失 fops、exclusive、controller 转发、日志行尾、锁/端口。

## 目录

```text
include/elib_drv.h        唯一公共头：错误码/树/fops/句柄/port/API/日志
src/elib_drv_internal.h   内部单例、锁助手、日志宏
src/elib_drv_manager.c    树校验/句柄填充/init/deinit/查找/遍历/日志设置
src/elib_drv_tree.c       节点计数、树查找、of API
src/elib_drv_node.c       统一 I/O、节点锁、句柄访问器
test/test_elib_drv.c      功能 + 生命周期 + 日志 + 端口回归
test/test_elib_drv_mt.c   POSIX 多线程验证（-pthread）
scripts/dt_check.py       设备树 C 源检查/展开表工具（Python 3）
docs/usage_*.md           6 篇流程文档（每篇配 .en.md 英文版）
```

## License

MIT License
