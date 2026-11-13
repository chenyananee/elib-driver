[简体中文](README.md) | **English**

# elib-driver

A device-tree driver framework for MCU targets: one static tree describes every
controller/device node, each node name is unique, and every node has one unified
`fops` table and one `void *ctx`. `elib_drv_init_tree()` automatically
initializes all nodes with each controller before its dependent nodes. Device
I/O and direct controller access share one `open/close/read/write/ioctl` API.

- C99, zero dynamic memory; the library never calls `malloc/free`. The tree,
  handle array, ctx objects and locks are supplied by the caller.
- One public header `include/elib_drv.h`; no private headers, no capacity macros.
- Unified nodes: no controller/device type split. A node with children can act
  as a controller; a node with a controller/parent relation is a device.
- Unified fops: one `init/deinit/open/close/read/write/ioctl` table.
- Unified I/O: `elib_drv_read/write/ioctl(node, ...)` works on any node. A
  device driver looks up its controller by name and calls the same API.
- RTOS agnostic: `elib_drv_port_t` provides mutex create/destroy/lock/unlock;
  `NULL` port means bare metal.
- Optional printf-style flow log, printed outside locks, with line-ending
  selection and full compile-time removal.

Current version: **2.0.0** (`ELIB_DRV_VERSION`).

## Features

- **The device tree is the only configuration source**: node `name`, `fops`,
  `ctx`, `controller`, `flags`, lock timeout and children all live in the tree;
  there is no descriptor/static/runtime registration table.
- **Unique node names**: `elib_drv_node_find()`, `elib_drv_of_find()` and a
  device looking up its controller all use the node name.
- **Automatic controller-first init**: `init_tree` recursively initializes the
  controller first and then the dependent node; a node gets exactly one
  `fops->init` per init run. Failures roll back in reverse order.
- **Idempotent fops**: `init/deinit` may run again on a deinit→init cycle; user
  callbacks must tolerate repeated calls.
- **Unified I/O**: `open/close` only call optional `fops->open/close` and maintain
  the reference count/exclusive flag. `read/write/ioctl` work after node init
  without requiring a prior open.
- **Direct controller access**: UART, bare ADC, GPIO and similar nodes can call
  `elib_drv_read/write/ioctl` on the controller node directly.
- **Device forwarding**: a device fops uses
  `elib_drv_node_find(elib_drv_node_controller_name(node))` and then calls the
  same I/O API; the framework holds the controller node lock.
- **of-style queries**: `elib_drv_of_find/of_config/of_name/of_controller/
  of_children`.
- **Offline checker**: `scripts/dt_check.py` expands scattered children arrays
  into one table and checks duplicates, controllers, depth and handle capacity
  without compiling.
- **Zero dynamic memory**: the node handle array is static or sized by
  `elib_drv_dt_node_count()` and allocated by the caller.

## Quick Start

```c
#include "elib_drv.h"

/* ---- 1. node ctx: hardware config/state, one void* ---- */
typedef struct { uint32_t base; uint32_t baud; } uart_cfg_t;
typedef struct { uint32_t addr; } sensor_cfg_t;

static uart_cfg_t   uart0_cfg  = { 0x40011000u, 115200u };
static sensor_cfg_t sensor_cfg = { 0x20u };

/* ---- 2. unified fops ---- */
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

    (void)cfg;                 /* target/register selection is controller specific */
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

/* ---- 3. device tree ---- */
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

/* ---- 4. init: the handle count can be computed first ---- */
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

    /* direct controller access */
    uart = elib_drv_node_find("uart0");
    (void)elib_drv_open(uart, 0u);
    (void)elib_drv_write(uart, "boot\r\n", 6u);
    (void)elib_drv_close(uart);

    /* device: looks up its controller and forwards */
    sensor = elib_drv_node_find("rhsensor");
    (void)elib_drv_read(sensor, buf, sizeof buf);

    (void)elib_drv_deinit();
    return 0;
}
```

Allocating the handle array at runtime (caller still owns the memory):

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

## Device Tree Check Script

Without compiling or running firmware, parse the `elib_drv_dt_node_t` arrays
directly from C source:

```bash
python scripts/dt_check.py src/app_dt.c --root roots
```

It prints a complete node table (name, depth, controller, fops, ctx, flags,
children, source line) and checks duplicate names, missing controllers,
controller cycles, undeclared children arrays, `child_count` mismatches, depth
and handle capacity.

```bash
# auto-select root arrays
python scripts/dt_check.py src/app_dt.c

# JSON/CSV for CI or spreadsheet tools
python scripts/dt_check.py src/app_dt.c --root roots --format json
python scripts/dt_check.py src/app_dt.c --root roots --format csv

# check handle capacity; make warnings fatal
python scripts/dt_check.py src/app_dt.c --root roots --capacity 8 --strict
```

See [docs/usage_device_tree.en.md](docs/usage_device_tree.en.md#offline-check-script).

## Architecture and Call Path

```text
elib_drv_init_tree()
  -> validate tree/names/depth/controller references
  -> count nodes and check the caller handle capacity
  -> fill handles preorder, create one lock per node
  -> recursively init_node(controller) -> fops->init(node)
  -> on failure roll back in reverse init_order

user/device I/O:
  elib_drv_read/write/ioctl(node)
    -> lock the node
    -> dispatch node->dt->fops
    -> unlock

device forwarding:
  inside device fops
    -> elib_drv_node_controller_name(node)
    -> elib_drv_node_find(name)
    -> elib_drv_read/write/ioctl(controller_node)
      -> lock the controller node, dispatch controller fops
```

## Core API

| API | Description |
|-----|-------------|
| `elib_drv_dt_node_count(tree, &n)` | Count tree nodes, useful to size the handle array |
| `elib_drv_init_tree(port, tree, nodes, cap)` | Validate, create locks, auto-init all nodes |
| `elib_drv_deinit()` | Check open counts, deinit in reverse, destroy locks |
| `elib_drv_node_find(name)` | Find the runtime handle by node name |
| `elib_drv_node_foreach(fn, arg)` | Walk every node |
| `elib_drv_open/close(node, ...)` | Optional open/close with refcount and exclusive flag |
| `elib_drv_read/write/ioctl(node, ...)` | Unified node I/O |
| `elib_drv_node_lock/unlock` | Explicit lock for grouping controller operations |
| `elib_drv_node_name/ctx/controller/...` | Handle accessors |
| `elib_drv_of_find/config/name/controller/children` | Device tree query API |

See `elib_drv_err_t` for error codes. `read/write` return byte counts; a driver
result greater than `len` is mapped to `-ELIB_DRV_ERR_IO`.

## Initialization and Lifetime

- `elib_drv_dt_node_count()`: validates children pointers/counts, depth
  (≤ 8) and counts nodes; it does not check duplicate names or controller
  references. An empty tree returns 0.
- `elib_drv_init_tree()`: requires an uninitialized manager; node count must be
  ≤ `node_capacity`. Every node gets one `fops->init` call (if present), with
  controllers before dependent nodes. On failure the original error is returned;
  successful nodes are deinitialized in reverse order and the manager resets.
- `elib_drv_deinit()`: returns `-BUSY` if any node has `open_count != 0`;
  calls `deinit` in reverse init order, destroys locks, keeps the log setting,
  and allows a later init.
- `fops->init/deinit/open/close` must be idempotent: init runs again after a
  deinit/re-init cycle, and open/close can run for multiple cycles.

## Locks, Concurrency and Controller Forwarding

- One lock per node; timeout is `lock_timeout_ms`, where 0 means
  `ELIB_DRV_WAIT_FOREVER`.
- `read/write/ioctl` call fops under the node lock; `open/close` do the same.
- Device forwarding uses `device node lock -> controller node lock`; the reverse
  order is forbidden, and a node's fops must not call I/O on the same node.
- Controller fops already hold the controller lock when called through a device;
  they must not lock the same node again.
- `elib_drv_node_lock/unlock` lets a driver group several controller operations.

## Flow Log

```c
elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);
```

- 0 = no line ending, 1 = `\n` (recommended), 2 = `\r\n`; invalid values return
  `-ELIB_DRV_ERR_INVALID_PARAM`.
- The callback runs outside framework locks; it must be thread safe and must not
  call `elib_drv_*`.
- `-DELIB_DRV_LOG_ENABLE=0` strips all log calls; the `set_log` API remains.
- Logs cover init/deinit/open/close/failures/lock timeouts; read/write/ioctl are
  not logged.

## RTOS Porting

`elib_drv_port_t` has four callbacks: `mutex_create/destroy/lock/unlock`.
`NULL` port means bare metal without locking. See
[docs/usage_porting.en.md](docs/usage_porting.en.md) for a complete example.

## Build and Test

No build system: add `include/` to the include path and `src/*.c` to the project.

```bash
# compile the library
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_tree.c src/elib_drv_node.c

# functional regression (public header only)
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv && ./t_drv

# log-stripped build
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c

# concurrency check (POSIX pthread, not part of the default build)
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt && ./t_mt
```

Coverage: tree counting/depth/validation, automatic controller-first init,
failure rollback, idempotent re-init, unified I/O, missing fops, exclusive,
controller forwarding, log line endings and port/lock behavior.

## Layout

```text
include/elib_drv.h        single public header: errors/tree/fops/handles/port/API/log
src/elib_drv_internal.h   internal singleton, lock helper, log macros
src/elib_drv_manager.c    tree validation/handle fill/init/deinit/find/foreach/set_log
src/elib_drv_tree.c       node counting, tree lookup, of API
src/elib_drv_node.c       unified I/O, node locks, handle accessors
test/test_elib_drv.c      functional + lifecycle + log + port regression
test/test_elib_drv_mt.c   POSIX multithread check (-pthread)
scripts/dt_check.py       device-tree C source checker/table tool (Python 3)
docs/usage_*.md           6 flow documents (each with an .en.md English version)
```

## License

MIT License
