[简体中文](usage_logging.md) | **English**

# Flow Logging

[Back to README](../README.en.md)

The framework offers an optional printf-style flow log: register an
`int (*)(const char *, ...)` callback and the framework prints init, deinit,
open, close, failure reasons and lock timeouts outside its locks;
read/write/ioctl are not logged.

## Registration

```c
#include <stdio.h>

elib_drv_set_log(printf, ELIB_DRV_LOG_NEWLINE_LF);
```

```c
int elib_drv_set_log(elib_drv_log_fn fn, unsigned newline);
```

- `fn == NULL`: disable logging.
- `newline`:
  - `ELIB_DRV_LOG_NEWLINE_NONE` (0): append nothing
  - `ELIB_DRV_LOG_NEWLINE_LF` (1): append `\n` (recommended)
  - `ELIB_DRV_LOG_NEWLINE_CRLF` (2): append `\r\n`
- An invalid `newline` returns `-ELIB_DRV_ERR_INVALID_PARAM` and leaves the
  previous setting unchanged.
- The setting survives init/deinit.

## Callback Contract

- The callback receives a complete line; it must not add another newline.
- It is called outside framework locks and may be called concurrently by
  multiple tasks; it must be thread safe.
- It must not call any `elib_drv_*` API.
- Its return value is ignored.

## Logged Events

Currently:

- `init tree` start, node init failure, init done
- `deinit begin` / `deinit done`
- open / close success and failure
- node lock timeout
- exclusive open BUSY

read/write/ioctl are deliberately not logged to keep the data path quiet.

## Stripping

```bash
gcc -std=c99 -DELIB_DRV_LOG_ENABLE=0 -Iinclude -c src/*.c
```

- All log calls and format strings are stripped.
- `elib_drv_set_log()` remains callable (argument validation still applies) but
  produces no output.
- Suitable for release firmware.

## Custom Logger

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

The logger owns thread safety; the framework only guarantees one callback per
record and calls it outside its locks.
