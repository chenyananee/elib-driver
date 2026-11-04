**简体中文** | [English](usage_logging.en.md)

# 流程日志

框架提供一条可选的 printf 风格流程日志：注册一个 `int (*)(const char *, ...)` 回调，框架在锁外打印 init、注册、注销、open、close 与失败原因；read/write/ioctl 不打印。

## 启用与关闭

```c
elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);    /* 默认：追加 '\n' */
elib_drv_set_log(my_log, ELIB_DRV_LOG_NEWLINE_CRLF);  /* 串口常用："\r\n" */
elib_drv_set_log(my_log, ELIB_DRV_LOG_NEWLINE_NONE);  /* 不追加行尾 */
elib_drv_set_log(NULL, ELIB_DRV_LOG_NEWLINE_LF);      /* 关闭输出 */
```

- `newline` 非法时返回 `-ELIB_DRV_ERR_INVALID_PARAM`，且不改变当前回调与行尾配置。
- 可以在 `elib_drv_init` 之前调用；回调与行尾配置跨 `init` / `deinit` 保留。
- 回调为 `NULL` 时不产生任何日志调用；日志只依赖 manager 里的函数指针，不需要额外内存。

## 行尾格式

| 参数 | 追加内容 | 说明 |
|---|---|---|
| `ELIB_DRV_LOG_NEWLINE_NONE`（0） | 无 | 回调收到未终止的一行 |
| `ELIB_DRV_LOG_NEWLINE_LF`（1） | `\n` | 默认推荐 |
| `ELIB_DRV_LOG_NEWLINE_CRLF`（2） | `\r\n` | 串口终端常用 |

- 行尾在设置时一起决定；每条记录只调用一次回调，回调收到完整一行。
- 使用 LF/CRLF 时，日志回调**不要**再自行补换行，否则会多出空行。
- 框架只追加 `\n` 或 `\r\n`，不做平台自动转换。

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
- 每条日志是一次单独的回调调用，回调返回值被忽略；行尾由配置追加到同一次调用里。

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

- `ELIB_DRV_LOG_ENABLE=0` 会把全部日志调用与格式串裁掉，`elib_drv_set_log` 仍可调用（仍需传 `newline`）但不再产生输出。
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
    elib_drv_set_log(uart_log, ELIB_DRV_LOG_NEWLINE_LF);  /* init 之前注册 */
    return elib_drv_init(NULL, &cfg);
}
```

## 相关

- [生命周期与初始化](usage_lifecycle.md)
- [锁、并发与超时](usage_locking.md)
- [RTOS 移植](usage_porting.md)
- [返回 README](../README.md)
