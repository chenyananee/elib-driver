**简体中文** | [English](usage_device_tree.en.md)

# 设备树模型

[返回 README](../README.md)

elib-driver 2.0 中，设备树是唯一的配置来源。树是静态 `const` 数据，通常放在
flash；运行时每个节点对应一个用户提供的 `elib_drv_node_t` 句柄。

## 树结构

```text
root
├── uart0                 controller 节点
│   └── rhsensor          device 节点，controller = "uart0"
└── i2c1                  controller 节点
    └── eeprom            device 节点，controller = "i2c1"
```

- 根 children 可以只放 controller、只放 device，或两者混放；空树也合法。
- 任意节点都可以有 children；实际使用时通常由 controller 挂 device。
- 节点没有 type 字段：有 children 的节点可充当 controller，有 `controller`
  或父关系的节点是 device；所有节点都能被 `open/read/write/ioctl`。
- 嵌套深度上限 `ELIB_DRV_DT_MAX_DEPTH`（8 层，根 children 算第 1 层）。
  这是**层数**限制，不是 children 数量限制：一个 controller 下挂 100 个设备，
  深度仍是 2，只受句柄总数/内存限制。

## 节点字段

```c
typedef struct elib_drv_dt_node {
    const char    *name;
    const elib_drv_fops_t *fops;
    void          *ctx;
    const char    *controller;
    uint32_t       flags;
    uint32_t       lock_timeout_ms;
    const struct elib_drv_dt_node *children;
    unsigned       child_count;
} elib_drv_dt_node_t;
```

| 字段 | 说明 |
|------|------|
| `name` | 全局唯一，必填；find/controller 引用的键 |
| `fops` | 统一 fops，可为 NULL |
| `ctx` | 唯一用户指针；硬件配置/状态都放这里，驱动自行强转 |
| `controller` | 可选 controller 节点名；为 NULL 时若为子节点则用父节点 |
| `flags` | `ELIB_DRV_DEV_F_*`；当前支持 `EXCLUSIVE` |
| `lock_timeout_ms` | 节点锁超时；0 表示 `ELIB_DRV_WAIT_FOREVER` |
| `children` | 子节点数组，可为 NULL |
| `child_count` | 子节点数量；children 非 NULL 时可为 0 |

## controller 解析

1. `controller` 字符串非空：按名字查找节点，找不到返回 `-NOT_FOUND`；
   指向自己返回 `-INVALID_PARAM`。
2. `controller == NULL` 且节点是某个节点的子节点：使用父节点。
3. 否则未绑定；device fops 内查找 controller 会得到 NULL。

节点名必须非空且全局唯一；重复返回 `-ALREADY_EXIST`。

## 节点计数与句柄数组

`elib_drv_dt_node_count()` 统计整棵树的节点数，可在 init 前调用：

```c
unsigned n = 0u;

if (elib_drv_dt_node_count(&tree, &n) != 0) {
    return -1;
}
```

静态句柄数组：

```c
static elib_drv_node_t g_nodes[8];

rc = elib_drv_init_tree(NULL, &tree, g_nodes,
                        ELIB_DRV_ARRAY_SIZE(g_nodes));
```

动态申请（用户负责释放，库不调用 malloc/free）：

```c
elib_drv_node_t *nodes = (n != 0u) ? malloc(n * sizeof(*nodes)) : NULL;

if (n != 0u && nodes == NULL) {
    return -1;
}
rc = elib_drv_init_tree(NULL, &tree, nodes, n);
if (rc != 0) {
    free(nodes);
    return rc;
}
/* ... 使用 ... */
elib_drv_deinit();
free(nodes);
```

- `node_capacity` 是句柄数组元素个数，必须 ≥ 树节点总数，否则 `-NO_SLOT`。
- 句柄数组必须从 init 前存活到 deinit 完成后。
- `node_count` 只做结构/深度校验，不检查重名和 controller 引用。

## 完整示例

```c
static const elib_drv_dt_node_t uart_children[] = {
    { .name = "rhsensor", .fops = &sensor_fops, .ctx = &sensor_cfg,
      .controller = "uart0", .lock_timeout_ms = 0u },
};

static const elib_drv_dt_node_t i2c_children[] = {
    { .name = "eeprom", .fops = &eeprom_fops, .ctx = &eeprom_cfg,
      .controller = "i2c1", .flags = ELIB_DRV_DEV_F_EXCLUSIVE,
      .lock_timeout_ms = 100u },
};

static const elib_drv_dt_node_t roots[] = {
    { .name = "uart0", .fops = &uart_fops, .ctx = &uart0_cfg,
      .children = uart_children,
      .child_count = ELIB_DRV_ARRAY_SIZE(uart_children),
      .lock_timeout_ms = 100u },
    { .name = "i2c1", .fops = &i2c_fops, .ctx = &i2c1_cfg,
      .children = i2c_children,
      .child_count = ELIB_DRV_ARRAY_SIZE(i2c_children),
      .lock_timeout_ms = 100u },
};

static const elib_drv_dt_t tree = {
    .children = roots, .child_count = ELIB_DRV_ARRAY_SIZE(roots),
};
```

## of 风格查询

```c
const elib_drv_dt_node_t *dt = elib_drv_of_find("i2c1");
const i2c_cfg_t *cfg = (const i2c_cfg_t *)elib_drv_of_config(dt);
unsigned child_count = 0u;
const elib_drv_dt_node_t *children =
    elib_drv_of_children(dt, &child_count);
```

- `elib_drv_of_find()` 需要 manager 已初始化；未初始化返回 NULL。
- `of_config()` 返回节点 `ctx`，节点为 NULL 时返回 NULL。
- `of_controller()` 返回节点里配置的 controller 字符串（不是解析后的父节点名）。
- 运行时解析后的 controller 用 `elib_drv_node_controller()` /
  `elib_drv_node_controller_name()` 获取。

## 离线检查脚本

`scripts/dt_check.py` 不需要编译，直接解析 C 源里的 `elib_drv_dt_node_t` 数组，
把零散的 children 数组展开成树状结构：

```bash
python scripts/dt_check.py src/app_dt.c --root roots
```

输出：

```text
roots
+- uart0 fops=&uart_fops ctx=&uart0_cfg lock_timeout_ms=100u child_count=ELIB_DRV_ARRAY_SIZE(uart_children)
|  `- rhsensor fops=&sensor_fops ctx=&sensor_cfg controller=uart0 lock_timeout_ms=0u
`- i2c1 fops=&i2c_fops ctx=&i2c1_cfg lock_timeout_ms=100u child_count=ELIB_DRV_ARRAY_SIZE(i2c_children)
   `- eeprom fops=&eeprom_fops ctx=&eeprom_cfg controller=i2c1 flags=ELIB_DRV_DEV_F_EXCLUSIVE lock_timeout_ms=100u
```

输出只保留静态数组字段：`name`、`fops`、`ctx`、`controller`、`flags`、
`lock_timeout_ms`、`child_count`，以及嵌套 `children`。JSON 是嵌套数组，
CSV 是同一组字段的行式输出。`Root arrays`、`Node count`、`Max depth`、建议
句柄大小、错误和警告都写到 stderr，所以 `> report.json` 只得到树本身。

检查内容包括：

- 重复节点名、缺失 controller、controller 自引用/成环；
- children 数组未声明、`child_count` 与实际数组长度不一致；
- 深度超过 8；
- 句柄数量超过 `--capacity`。

常用参数：

```bash
# 自动选择未被其他节点 children 引用的根数组
python scripts/dt_check.py src/app_dt.c

# 同时检查多个文件
python scripts/dt_check.py src/app_dt.c src/board_dt.c --root roots

# JSON / CSV，便于 CI 或表格工具
python scripts/dt_check.py src/app_dt.c --root roots --format json
python scripts/dt_check.py src/app_dt.c --root roots --format csv

# 顺便检查句柄容量；警告也作为失败
python scripts/dt_check.py src/app_dt.c --root roots --capacity 8 --strict
```

脚本支持 `.name = ...` 指定初始化器和按 `elib_drv_dt_node_t` 字段顺序的位置
初始化器；不执行宏，只识别 `ELIB_DRV_ARRAY_SIZE(数组名)` 和整数常量。

## 注意

- 树、节点名、`ctx`、fops 表必须比 manager 生命周期长。
- 节点名是唯一键；不要用会释放的栈缓冲区作为 name。
- 节点 `ctx` 只有一个指针，配置和可变状态由驱动自行组织。
- 树深度超过 8 层时 init/count 返回 `-INVALID_PARAM`；children 成环也会在
  init 深度检查中被拒绝。
