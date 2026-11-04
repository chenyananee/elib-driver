**简体中文** | [English](usage_logging.en.md)

# 流程日志

框架提供一条可选的 printf 风格流程日志：注册一个 `int (*)(const char *, ...)` 回调，框架在锁外打印 init、注册、注销、open、close 与失败原因；read/write/ioctl 不打印。

## 启用与关闭

```c
elib_drv_set_log(printf);   /* 最简单：直接接 printf */
elib_drv_set_log(my_log);   /* 或自己的 UART / RTT logger */
elib_drv_set_log(NULL);     /* 关闭 */
```

- `elib_drv_set_log` 在任何时候都返回 0，可以在 `elib_drv_init` 之前调用。
- 回调跨 `init` / `deinit` 保留：`deinit` 不会把它清掉，重新 init 后继续生效。
- 回调为 `NULL` 时不产生任何日志调用；日志只依赖 manager 里的函数指针，不需要额外内存。

## 记录点

| 阶段 | 级别 | 内容 |
|---|---|---|
| init 开始 | I | 静态/运行时表项数量 |
| init 成功 | I | `init ok` |
| init 失败 | E | 错误码（建锁失败、controller init 失败） |
| controller 注册成功 | I | controller 名字 |
| controller 注册失败 | W | 名字 + 错误码（重名/无槽/建锁） |
| controller init 失败 | E | 名字 + 错误码 |
| controller 注销成功 | I | controller 名字 |
| controller 注销失败 | W | 名字 + 错误码（BUSY） |
| controller deinit 失败 | E | 名字 + 错误码 |
| device 注册成功 | I | device 名字 |
| device 注册失败 | W | 名字 + 错误码（重名/controller 不存在/无槽/建锁） |
| device 注销成功 / 失败 | I / W | 名字（+ 错误码） |
| device open / close | I | 设备名 |
| device open / close 失败 | W / E | 设备名 + 错误码 |
| controller 直连 open / close | I | controller 名 |
| controller 直连 open / close 失败 | E | controller 名 + 错误码 |
| deinit busy | W | 固定 `deinit busy` |
| 锁超时 | E | device/controller 名 |

> `read` / `write` / `ioctl`（含直连）不产生日志；不要在驱动里依赖框架日志做逐次追踪。

## 格式约定

- 固定前缀 `"[elib_drv] I|W|E: ..."`；`I` 信息、`W` 警告、`E` 错误。
- 框架只用 `%s`、`%u`、`%d` 三种转换，名字与错误码都由框架传入。
- 每条日志是一次单独的回调调用，回调返回值被忽略。

## 线程与重入

- 日志在**释放锁之后**打印，回调内不会持有框架锁。
- 不同设备的 open/close 可能在不同任务里同时触发日志，回调必须线程安全。
- 回调内**不得**调用任何 `elib_drv_*`（防递归/重入）。
- 日志时机不影响 API 返回值；日志失败不会改变流程结果。

## 编译期裁剪

```bash
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c
```

- `ELIB_DRV_LOG_ENABLE=0` 会把全部日志调用与格式串裁掉，`elib_drv_set_log` 仍可调用但不再产生输出。
- 默认 `ELIB_DRV_LOG_ENABLE=1`；测试里的日志断言随该宏自动开关。

## 示例

```c
#include <stdarg.h>
#include <stdio.h>

static int uart_log(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) {
        uart_write(line, (unsigned)n < sizeof line ? (unsigned)n : sizeof line - 1);
    }
    return 0;
}

int app_boot(void)
{
    elib_drv_set_log(uart_log);          /* init 之前注册 */
    return elib_drv_init(NULL, &cfg);
}
```

## 相关

- [生命周期与初始化](usage_lifecycle.md)
- [锁、并发与超时](usage_locking.md)
- [RTOS 移植](usage_porting.md)
- [返回 README](../README.md)
