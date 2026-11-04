**简体中文** | [English](usage_build_test.en.md)

# 构建与测试

库没有构建系统：把 `include/` 加进头文件路径、把 `src/*.c` 加进工程即可。本页给出目标工具链的零警告构建矩阵，以及功能/并发/裁剪三类测试的跑法。

## 源码列表

```text
include/elib_drv.h       唯一公共头
src/elib_drv_internal.h  内部单例、锁助手、日志宏
src/elib_drv_manager.c   init/deinit/校验/双表/注册注销/查找/遍历
src/elib_drv_device.c    open/close/read/write/ioctl/访问器/控制器锁
```

## ARM 工具链构建（硬性要求）

验证工具链：`C:\SF\arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-arm-none-eabi`（Arm GNU Toolchain 15.2.Rel1，`arm-none-eabi-gcc` 15.2.1）。

要求：`-std=c99 -Wall -Wextra -Werror -pedantic` 下零警告零错误；`-O0` / `-O2` / `-Os` 与日志开/关都要通过。

```bash
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_manager.c -o elib_drv_manager.o
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_device.c -o elib_drv_device.o
```

日志裁剪与优化级别各跑一遍：

```bash
for opt in -O0 -O2 -Os; do
    arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
        -mcpu=cortex-m4 -mthumb "$opt" -Iinclude \
        -c src/elib_drv_manager.c src/elib_drv_device.c
    arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
        -mcpu=cortex-m4 -mthumb "$opt" -DELIB_DRV_LOG_ENABLE=0 -Iinclude \
        -c src/elib_drv_manager.c src/elib_drv_device.c
done
```

> 功能测试源码也可编译成对象文件做语法/警告检查；它依赖 `stdio.h`，不链接进目标镜像。

## 功能测试

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/elib_drv_manager.c src/elib_drv_device.c \
    -o t_drv && ./t_drv
```

- 只包含公共头 `elib_drv.h`，不碰内部头；
- 自带 `main()`，失败用例逐条打印 `FAIL 文件:行`，全部通过时退出码为 0。

## 并发测试

```bash
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c \
    src/elib_drv_manager.c src/elib_drv_device.c -o t_mt && ./t_mt
```

- 用 pthread 端口验证 device I/O 与直连 write 共享 controller 锁不交错；
- 同时验证 `ELIB_DRV_DEV_F_EXCLUSIVE` 在并发 open 下只放行一个；
- 属于 POSIX 宿主测试，不属于目标固件构建。

## 日志裁剪

```bash
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/elib_drv_manager.c src/elib_drv_device.c
```

- 全部日志调用与格式串被裁掉，`elib_drv_set_log` 仍可调用；
- 测试里的日志断言随 `ELIB_DRV_LOG_ENABLE` 自动关闭，功能测试仍应全绿。

## 预期输出

功能测试：

```text
=== elib-driver tests ===
Test: test_init_wires_static... done
...
ALL PASSED (0 failures)
```

并发测试：

```text
MT ALL PASSED (300000 hardware ops, 1 exclusive winner)
```

## 测试覆盖

| 领域 | 覆盖内容 |
|---|---|
| init / deinit | 静态接线、NULL port、非法配置、重名、空 ctx、未知 controller、重复 init、init 中 deinit 拒绝、逆序 teardown、失败 controller 直连关闭、deinit BUSY、回滚、deinit 错误透传 |
| 查找 / 遍历 | `find`、`foreach` 顺序与提前停止 |
| device I/O | 引用计数、EXCLUSIVE、open 失败、未打开、参数/返回值检查、NOT_SUPPORTED、I/O 不打日志、锁超时 |
| 控制器直连 | 引用计数、I/O、未打开、无 fops、参数/返回检查、持锁调用、BUSY、失败回滚、访问器 |
| 运行时表 | 无槽位、注册/注销、重复注销拒绝、init 内注销自身拒绝、重名、未知 controller、槽位复用、未对齐/极小缓存、init 子设备与未绑定设备回收、槽位耗尽 |
| 日志 | 流程记录、关闭、deinit 保留、注册失败日志、直连日志 |
| 边界 | 上下文尺寸上界、访问器 NULL 安全 |

## 已知限制

- `test/test_elib_drv_mt.c` 依赖 `pthread.h`，`arm-none-eabi` 目标工具链没有该头文件，不能用目标工具链编译；
- 本页的功能/并发命令需要宿主 gcc（MinGW、Linux 或 WSL 均可）；
- 库只用到 `<string.h>` 的 `memset`/`strcmp`，由编译器的 C 运行时或工程提供；目标工程只需要 `src/*.c` 与 `include/`。

## 相关

- [生命周期与初始化](usage_lifecycle.md)
- [流程日志](usage_logging.md)
- [RTOS 移植](usage_porting.md)
- [返回 README](../README.md)
