**简体中文** | [English](usage_logging.en.md)

# 流程日志

[返回 README](../README.md)

框架提供一条可选的 printf 风格流程日志：注册一个
`int (*)(const char *, ...)` 回调，框架在锁外打印 init、deinit、open、close、
失败原因和锁超时；read/write/ioctl 不打印。

## 注册

```c
#include <stdio.h>

elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);
```

```c
int elib_drv_set_log(elib_drv_log_fn fn, unsigned newline);
```

- `fn == NULL`：关闭日志。
- `newline`：
  - `ELIB_DRV_LOG_NEWLINE_NONE`（0）：不追加行尾
  - `ELIB_DRV_LOG_NEWLINE_LF`（1）：追加 `\n`（推荐）
  - `ELIB_DRV_LOG_NEWLINE_CRLF`（2）：追加 `\r\n`
- 非法 `newline` 返回 `-ELIB_DRV_ERR_INVALID_PARAM`，原配置保持不变。
- 设置跨 init/deinit 保留。

## 回调约定

- 回调收到完整一行，不需要自己再加换行。
- 在框架锁外调用；可能被多个任务并发调用，必须线程安全。
- 回调内不得调用任何 `elib_drv_*` API。
- 返回值被忽略。

## 日志内容

当前覆盖：

- `init tree` 开始、节点 init 失败、init 完成
- `deinit begin` / `deinit done`
- open / close 成功与失败
- 节点锁超时
- 独占 open BUSY

read/write/ioctl 本身不打印，避免数据路径抖动。

## 裁剪

```bash
gcc -std=c99 -DELIB_DRV_LOG_ENABLE=0 -Iinclude -c src/*.c
```

- 所有日志调用与格式串被整段裁掉。
- `elib_drv_set_log()` 仍可调用（参数校验仍生效），但不会产生输出。
- 适合 release 固件。

## 自定义 logger

```c
static int uart_logger(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        uart_write_blocking(line, (size_t)n);
    }
    return n;
}

elib_drv_set_log(uart_logger, ELIB_DRV_LOG_NEWLINE_CRLF);
```

logger 必须自己保证线程安全；框架只保证“一条日志一次回调、锁外调用”。
