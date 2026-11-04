[简体中文](usage_build_test.md) | **English**

# Building and Testing

The library has no build system: add `include/` to the include path and `src/*.c` to the project. This page gives the zero-warning build matrix for the target toolchain plus the functional, concurrency and stripped-log test runs.

## Source list

```text
include/elib_drv.h       the only public header
src/elib_drv_internal.h  private singleton, lock helpers, log macro
src/elib_drv_manager.c   init/deinit/validation/tables/registration/lookup
src/elib_drv_device.c    open/close/read/write/ioctl/accessors/controller lock
```

## ARM toolchain build (hard requirement)

Verified toolchain: `C:\SF\arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-arm-none-eabi` (Arm GNU Toolchain 15.2.Rel1, `arm-none-eabi-gcc` 15.2.1).

Requirement: zero warnings and zero errors under `-std=c99 -Wall -Wextra -Werror -pedantic`; `-O0` / `-O2` / `-Os` with the log both on and off must pass.

```bash
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_manager.c -o elib_drv_manager.o
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_device.c -o elib_drv_device.o
```

Run the stripped-log variant and every optimization level:

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

> The functional test source may also be compiled to an object file for syntax/warning checks; it depends on `stdio.h` and is not linked into the target image.

## Functional tests

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/elib_drv_manager.c src/elib_drv_device.c \
    -o t_drv && ./t_drv
```

- Includes only the public `elib_drv.h`, never a private header;
- Ships its own `main()`; a failing case prints `FAIL file:line`, and the exit code is 0 when everything passes.

## Concurrency test

```bash
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c \
    src/elib_drv_manager.c src/elib_drv_device.c -o t_mt && ./t_mt
```

- A pthread port verifies that device I/O and a direct write sharing the controller lock never interleave;
- It also verifies that `ELIB_DRV_DEV_F_EXCLUSIVE` admits exactly one concurrent opener;
- This is a POSIX host test, not part of the target firmware build.

## Stripped log

```bash
gcc -std=c99 -Wall -Wextra -Werror -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude -c src/elib_drv_manager.c src/elib_drv_device.c
```

- Every log call and format string is stripped; `elib_drv_set_log` remains callable;
- The log assertions in the test suite switch off with `ELIB_DRV_LOG_ENABLE`, and the functional suite must still pass.

## Expected output

Functional tests:

```text
=== elib-driver tests ===
Test: test_init_wires_static... done
...
ALL PASSED (0 failures)
```

Concurrency test:

```text
MT ALL PASSED (300000 hardware ops, 1 exclusive winner)
```

## Test coverage

| Area | Covered |
|---|---|
| init / deinit | static wiring, NULL port, invalid config, duplicates, null ctx, unknown controller, double init, deinit rejected during init, reverse teardown, failing controller direct-close, deinit BUSY, rollback, deinit error propagation |
| lookup / iteration | `find`, `foreach` order and early stop |
| device I/O | reference counting, EXCLUSIVE, open failure, not open, argument/result checks, NOT_SUPPORTED, I/O not logged, lock timeout |
| direct controller | reference counting, I/O, not open, no fops, argument/result checks, lock held, BUSY, rollback, accessors |
| runtime table | no slot, register/unregister, double unregister rejected, self-unregister during init rejected, duplicates, unknown controller, slot reuse, unaligned/tiny cache, init child and unbound-device cleanup, slot exhaustion |
| logging | flow lines, disable, deinit retention, registration failure logs, direct logs |
| bounds | context size upper bounds, NULL-safe accessors |

## Known limitations

- `test/test_elib_drv_mt.c` depends on `pthread.h`, which the `arm-none-eabi` toolchain does not provide, so it cannot be compiled for the target;
- The functional/concurrency commands above need a host gcc (MinGW, Linux or WSL);
- The library only uses `memset`/`strcmp` from `<string.h>`, provided by the compiler's C runtime or the project; a target project only needs `src/*.c` and `include/`.

## See also

- [Lifecycle and initialization](usage_lifecycle.en.md)
- [Flow logging](usage_logging.en.md)
- [RTOS porting](usage_porting.en.md)
- [Back to README](../README.en.md)
