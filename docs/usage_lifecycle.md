**简体中文** | [English](usage_lifecycle.en.md)

# 生命周期与初始化

[返回 README](../README.md)

## 初始化入口

```c
int elib_drv_init_tree(const elib_drv_port_t *port,
                       const elib_drv_dt_t *tree,
                       elib_drv_node_t *nodes,
                       unsigned node_capacity);
```

- `port == NULL`：裸机/单线程，不创建锁。
- `tree`：静态设备树；节点名必须全局唯一。
- `nodes`/`node_capacity`：用户提供的运行时句柄数组及容量。
- 已初始化时再次调用返回 `-ALREADY_INIT`。

## 校验顺序

1. port：NULL 或四个回调齐全。
2. `elib_drv_dt_node_count()`：children 指针/数量、深度 ≤ 8、节点总数。
3. 节点总数 ≤ `node_capacity`；有节点时 `nodes != NULL`。
4. 预序填句柄：dt、父节点 controller、child_count、open_count=0、init 状态复位。
5. 节点名非空且全局唯一。
6. `controller` 字符串解析：显式名字优先，否则父节点；显式名字必须存在且
   不能指向自己。
7. 为每个节点创建锁（有 port 时）；失败销毁已创建锁并返回 `-LOCK`。

## 自动 controller-first 初始化

初始化从根节点数组开始，对每个尚未初始化的节点执行：

```text
init_node(n):
  DONE        -> return 0
  IN_PROGRESS -> 返回 INVALID_PARAM（controller 链成环）
  IN_PROGRESS = 1
  如果 n 有 controller:
      rc = init_node(controller)
      失败则状态复位并返回
  如果 n 有 fops->init:
      rc = fops->init(n)
      失败则状态复位并返回
  n->init_order = ++init_seq
  DONE
```

结果：

- controller 先于依赖它的节点。
- 共享 controller 只初始化一次。
- 一次 init 中同一节点只调用一次。
- init 过程中 manager 已标记 initialized，因此 fops->init 内可以
  `elib_drv_node_find()` / `elib_drv_of_find()` / 读 ctx。
- 无 `fops->init` 的节点也标记 DONE，子节点可继续。

## 失败回滚

任一节点 init 失败后：

1. 按 `init_order` 降序，对已 DONE 节点调用 `fops->deinit`。
2. 销毁所有节点锁。
3. 清空 manager 的 tree/nodes/count/init 状态。
4. 返回最初错误。

失败节点本身不会被 `deinit`（它的 init 未成功）；回滚只处理已成功节点。
fops->init 不应留下打开计数；如果 init 里 open 了节点，deinit 可能 BUSY。

## 反初始化

```c
int elib_drv_deinit(void);
```

- 未初始化返回 `-NOT_INITIALIZED`。
- 初始化进行中返回 `-BUSY`。
- 任一节点 `open_count != 0` 返回 `-BUSY`，不会部分拆除。
- 按 init 逆序调用 `fops->deinit`（子节点先于 controller）。
- 销毁锁，清空 manager；log 配置保留。
- fops->deinit 返回错误时继续拆完其它节点，返回第一个错误。

## 幂等与重复初始化

- deinit 后可以再次 init_tree；所有节点会再次收到 init。
- 未 deinit 时重复 init 返回 `-ALREADY_INIT`。
- fops->init/deinit/open/close 必须可重复调用；框架只保证一次 init 中不重复
  初始化同一节点，不替用户保证寄存器级幂等。
- 句柄数组和树可以在 re-init 时复用；init 会重新填句柄。

## 错误表

| 错误 | 场景 |
|------|------|
| `-INVALID_PARAM` | tree/port/节点名为空、children 指针/数量不一致、深度超限、controller 自引用、名字空 |
| `-ALREADY_INIT` | manager 已初始化 |
| `-ALREADY_EXIST` | 节点名重复 |
| `-NOT_FOUND` | controller 名字不存在 |
| `-NO_SLOT` | 节点数 > node_capacity，或 nodes 为空 |
| `-LOCK` | 创建锁失败 |
| `-BUSY` | deinit 时有打开节点 / init 进行中 |
| `-NOT_INITIALIZED` | 未初始化时调用 deinit/查询/I/O |
| 驱动错误 | fops->init 返回值原样返回 |
