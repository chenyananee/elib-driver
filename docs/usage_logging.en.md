[简体中文](usage_logging.md) | **English**

# Flow Logging

The framework offers an optional printf-style flow log: register an `int (*)(const char *, ...)` callback and the framework prints init, registration, unregistration, open, close and failure reasons outside its locks; read/write/ioctl are never logged.

## Enabling and disabling

```c
elib_drv_set_log(printf);   /* simplest: printf itself */
elib_drv_set_log(my_log);   /* or your own UART / RTT logger */
elib_drv_set_log(NULL);     /* disable */
```

- `elib_drv_set_log` always returns 0 and may be called before `elib_drv_init`.
- The callback survives `init` / `deinit`: `deinit` does not clear it, and it keeps working after a new init.
- A `NULL` callback produces no log calls at all; the log only costs one function pointer in the manager.

## Log points

| Phase | Level | Content |
|---|---|---|
| init start | I | static/runtime entry counts |
| init success | I | `init ok` |
| init failure | E | error code (lock creation, controller init) |
| controller registered | I | controller name |
| controller registration failed | W | name + error (duplicate/no slot/lock) |
| controller init failed | E | name + error |
| controller unregistered | I | controller name |
| controller unregistration failed | W | name + error (BUSY) |
| controller deinit failed | E | name + error |
| device registered | I | device name |
| device registration failed | W | name + error (duplicate/unknown controller/no slot/lock) |
| device unregistered / failed | I / W | name (+ error) |
| device open / close | I | device name |
| device open / close failed | W / E | device name + error |
| controller direct open / close | I | controller name |
| controller direct open / close failed | E | controller name + error |
| deinit busy | W | fixed `deinit busy` |
| lock timeout | E | device/controller name |

> `read` / `write` / `ioctl` (direct included) produce no log lines; do not depend on the framework log for per-call tracing.

## Format conventions

- Fixed prefix `"[elib_drv] I|W|E: ..."`; `I` info, `W` warning, `E` error.
- The framework only uses `%s`, `%u` and `%d`; names and error codes are supplied by the framework.
- Each line is one callback invocation; its return value is ignored.

## Threading and reentrancy

- Logs are emitted **after the lock is released**, so the callback never holds a framework lock.
- Opens/closes of different devices may log from different tasks at the same time; the callback must be thread safe.
- The callback must **not** call any `elib_drv_*` function (reentrancy).
- Logging never changes API return values; a failing log does not alter the flow.

## Compile-time stripping

```bash
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/*.c
```

- `ELIB_DRV_LOG_ENABLE=0` strips every log call and format string; `elib_drv_set_log` remains callable but produces no output.
- The default is `ELIB_DRV_LOG_ENABLE=1`; the log assertions in the test suite follow that macro automatically.

## Example

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
    elib_drv_set_log(uart_log);          /* register before init */
    return elib_drv_init(NULL, &cfg);
}
```

## See also

- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Locking and concurrency](usage_locking.en.md)
- [RTOS porting](usage_porting.en.md)
- [Back to README](../README.en.md)
