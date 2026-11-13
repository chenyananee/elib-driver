[简体中文](usage_lifecycle.md) | **English**

# Lifecycle and Initialization

[Back to README](../README.en.md)

## Init Entry

```c
int elib_drv_init_tree(const elib_drv_port_t *port,
                       const elib_drv_dt_t *tree,
                       elib_drv_node_t *nodes,
                       unsigned node_capacity);
```

- `port == NULL`: bare metal/single threaded; no locks are created.
- `tree`: static device tree; node names must be globally unique.
- `nodes`/`node_capacity`: caller supplied runtime handle array and capacity.
- Calling it while already initialized returns `-ALREADY_INIT`.

## Validation Order

1. port: NULL or all four callbacks present.
2. `elib_drv_dt_node_count()`: children pointers/counts, depth ≤ 8, total nodes.
3. Node count <= `node_capacity`; `nodes != NULL` when nodes exist.
4. Fill handles preorder: dt, parent controller, child_count, open_count=0, init
   state reset.
5. Node names non-empty and globally unique.
6. Resolve `controller`: explicit name first, otherwise parent; an explicit name
   must exist and must not point to itself.
7. Create one lock per node (when a port is supplied); on failure destroy the
   created locks and return `-LOCK`.

## Automatic Controller-First Init

Init starts at the root array and runs for every not-yet-initialized node:

```text
init_node(n):
  DONE        -> return 0
  IN_PROGRESS -> return INVALID_PARAM (controller chain cycle)
  IN_PROGRESS = 1
  if n has a controller:
      rc = init_node(controller)
      on failure reset state and return
  if n has fops->init:
      rc = fops->init(n)
      on failure reset state and return
  n->init_order = ++init_seq
  DONE
```

Consequences:

- A controller is initialized before dependent nodes.
- A shared controller is initialized once.
- A node gets one init call per init run.
- During init the manager is already marked initialized, so `fops->init` may
  call `elib_drv_node_find()` / `elib_drv_of_find()` and read ctx.
- A node without `fops->init` is marked DONE and its children can proceed.

## Failure Rollback

When any node init fails:

1. Walk `init_order` descending and call `fops->deinit` for DONE nodes.
2. Destroy all node locks.
3. Clear manager tree/nodes/count/init state.
4. Return the original error.

The failing node itself is not deinitialized because its init did not succeed;
rollback only covers successful nodes. `fops->init` must not leave open counts;
opening a node inside init can make deinit return BUSY.

## Deinitialization

```c
int elib_drv_deinit(void);
```

- Returns `-NOT_INITIALIZED` when not initialized.
- Returns `-BUSY` while init is running.
- Returns `-BUSY` when any node has `open_count != 0`; nothing is partially torn
  down.
- Calls `fops->deinit` in reverse init order (children before controllers).
- Destroys locks and clears the manager; the log setting survives.
- If a fops->deinit returns an error, the remaining nodes are still torn down
  and the first error is returned.

## Idempotency and Re-Init

- After deinit, init_tree may run again; all nodes receive init again.
- A second init without deinit returns `-ALREADY_INIT`.
- fops->init/deinit/open/close must tolerate repeated calls. The framework only
  guarantees one init per node per run; it does not make register writes
  idempotent for the driver.
- The tree and handle array may be reused; init refills the handles.

## Error Table

| Error | Case |
|-------|------|
| `-INVALID_PARAM` | NULL tree/port/node name, children pointer/count mismatch, depth exceeded, controller self reference, empty name |
| `-ALREADY_INIT` | manager already initialized |
| `-ALREADY_EXIST` | duplicate node name |
| `-NOT_FOUND` | controller name does not exist |
| `-NO_SLOT` | node count > node_capacity, or nodes is NULL |
| `-LOCK` | lock creation failed |
| `-BUSY` | deinit with open nodes / init in progress |
| `-NOT_INITIALIZED` | deinit/query/I/O before init |
| driver error | returned unchanged from fops->init |
