**简体中文** | [English](usage_build_test.en.md)

# 构建与测试

[返回 README](../README.md)

库没有构建系统：把 `include/` 加进头文件路径，把 `src/*.c` 加进工程即可。

## 源文件

```text
src/elib_drv_manager.c
src/elib_drv_tree.c
src/elib_drv_node.c
```

## ARM 目标构建

```bash
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_tree.c src/elib_drv_node.c
```

零警告矩阵：

- `src/*.c` × `-O0/-O2/-Os` × `ELIB_DRV_LOG_ENABLE=1/0` = 18 个对象
- `test/test_elib_drv.c` × 日志开/关 = 2 个对象
- 合计 20 个对象，全部 `-Wall -Wextra -Werror -pedantic` 零警告

## 宿主功能测试

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv
./t_drv
```

日志裁剪：

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude test/test_elib_drv.c src/*.c -o t_drv_nolog
./t_drv_nolog
```

期望输出：`ALL PASSED (n checks)`。

MSVC 示例：

```bat
cl /nologo /W4 /std:c11 /Iinclude test\test_elib_drv.c ^
   src\elib_drv_manager.c src\elib_drv_tree.c src\elib_drv_node.c ^
   /Fe:t_drv.exe
t_drv.exe
```

## 并发测试

POSIX pthread 环境：

```bash
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt
./t_mt
```

覆盖：

- device fops 内转发 `elib_drv_write(controller)` 与 controller 直连写共享
  controller 节点锁，硬件访问不交错；
- EXCLUSIVE 节点 8 个并发 open，仅 1 个成功。

该文件依赖 `<pthread.h>`，不属于 ARM 默认构建；在无 pthread 的 Windows MSVC
环境只做代码审查，不参与默认验证。

## 测试覆盖

`test/test_elib_drv.c` 覆盖：

- `dt_node_count`：空树、嵌套计数、空 children、深度超限、NULL 参数
- init：自动 controller-first、共享 controller、无 init 节点、失败回滚、
  幂等/reinit
- 树校验：重名、空名、controller 缺失/空/自引用、容量不足、坏 port
- I/O：未 open 读写、open/close 引用计数、exclusive、缺失 fops、参数校验、
  返回值 > len
- controller 转发：device 按名查 controller 并调用统一 I/O
- 日志：LF/CRLF/NONE、跨 init/deinit 保留、非法行尾
- port：锁创建/加锁/解锁计数、创建失败、锁失败

## 目录产物

建议把 `t_drv`、`t_drv_nolog`、`t_mt`、`*.o`、`*.obj`、`*.exe` 放在
`build/` 或由 `.gitignore` 忽略，不要提交构建产物。
