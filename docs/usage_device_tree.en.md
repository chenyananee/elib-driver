[简体中文](usage_device_tree.md) | **English**

# Device Tree Model

[Back to README](../README.en.md)

In elib-driver 2.0 the device tree is the only configuration source. The tree
is static `const` data, usually placed in flash. Each node has a corresponding
caller-provided `elib_drv_node_t` runtime handle.

## Tree Shape

```text
root
├── uart0                 controller node
│   └── rhsensor          device node, controller = "uart0"
└── i2c1                  controller node
    └── eeprom            device node, controller = "i2c1"
```

- Root children may contain only controllers, only devices, or both; an empty
  tree is valid.
- Any node may have children; in practice a controller usually lists devices.
- There is no node type field: a node with children can act as a controller, a
  node with a `controller` name or parent relation is a device, and every node
  can be opened/read/written/ioctl'd.
- Maximum nesting depth is `ELIB_DRV_DT_MAX_DEPTH` (8 levels; root children are
  level 1). This is a **depth** limit, not a children limit: a controller with
  100 child devices is still level 2 and is limited only by total handles/RAM.

## Node Fields

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

| Field | Description |
|-------|-------------|
| `name` | Globally unique, required; key for find/controller references |
| `fops` | Unified fops table, may be NULL |
| `ctx` | The only user pointer; hardware config/state lives here and the driver casts it |
| `controller` | Optional controller node name; NULL means parent node (if any) |
| `flags` | `ELIB_DRV_DEV_F_*`; currently `EXCLUSIVE` |
| `lock_timeout_ms` | Node lock timeout; 0 means `ELIB_DRV_WAIT_FOREVER` |
| `children` | Child array, may be NULL |
| `child_count` | Child count; may be 0 when children is NULL |

## Controller Resolution

1. Non-empty `controller`: look up the node by name; missing returns
   `-NOT_FOUND`; pointing to itself returns `-INVALID_PARAM`.
2. `controller == NULL` and the node is a child: use the parent node.
3. Otherwise unbound; a device looking up its controller gets NULL.

Names must be non-empty and globally unique; a duplicate returns
`-ALREADY_EXIST`.

## Node Counting and Handle Array

`elib_drv_dt_node_count()` counts all tree nodes and can be called before init:

```c
unsigned n = 0u;

if (elib_drv_dt_node_count(&tree, &n) != 0) {
    return -1;
}
```

Static handle array:

```c
static elib_drv_node_t g_nodes[8];

rc = elib_drv_init_tree(NULL, &tree, g_nodes,
                        ELIB_DRV_ARRAY_SIZE(g_nodes));
```

Dynamic allocation (the caller owns and frees the memory; the library never
calls malloc/free):

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
/* ... use ... */
elib_drv_deinit();
free(nodes);
```

- `node_capacity` is the number of handle entries and must be >= the tree node
  count, otherwise `-NO_SLOT`.
- The handle array must live from before init until after deinit.
- `node_count` checks only structure/depth, not duplicate names or controller
  references.

## Complete Example

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

## of-Style Queries

```c
const elib_drv_dt_node_t *dt = elib_drv_of_find("i2c1");
const i2c_cfg_t *cfg = (const i2c_cfg_t *)elib_drv_of_config(dt);
unsigned child_count = 0u;
const elib_drv_dt_node_t *children =
    elib_drv_of_children(dt, &child_count);
```

- `elib_drv_of_find()` requires an initialized manager; it returns NULL before
  init.
- `of_config()` returns the node `ctx`, or NULL for a NULL node.
- `of_controller()` returns the configured controller string, not the resolved
  parent name.
- For the resolved controller use `elib_drv_node_controller()` /
  `elib_drv_node_controller_name()`.

## Notes

- The tree, node names, `ctx` and fops tables must outlive the manager.
- Node names are the unique key; do not point them at stack buffers that go out
  of scope.
- A node has exactly one `ctx`; the driver decides how to organize config and
  mutable state behind it.
- Trees deeper than 8 levels make init/count return `-INVALID_PARAM`; child
  cycles are rejected by the init depth check.
