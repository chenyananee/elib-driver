[简体中文](usage_build_test.md) | **English**

# Building and Testing

[Back to README](../README.en.md)

The library has no build system: add `include/` to the include path and
`src/*.c` to the project.

## Sources

```text
src/elib_drv_manager.c
src/elib_drv_tree.c
src/elib_drv_node.c
```

## ARM Target Build

```bash
arm-none-eabi-gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -mcpu=cortex-m4 -mthumb -O2 -Iinclude \
    -c src/elib_drv_manager.c src/elib_drv_tree.c src/elib_drv_node.c
```

Zero-warning matrix:

- `src/*.c` × `-O0/-O2/-Os` × `ELIB_DRV_LOG_ENABLE=1/0` = 18 objects
- `test/test_elib_drv.c` × log on/off = 2 objects
- 20 objects total, all zero-warning with
  `-Wall -Wextra -Werror -pedantic`

## Host Functional Tests

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/test_elib_drv.c src/*.c -o t_drv
./t_drv
```

Log-stripped build:

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -DELIB_DRV_LOG_ENABLE=0 \
    -Iinclude test/test_elib_drv.c src/*.c -o t_drv_nolog
./t_drv_nolog
```

Expected output: `ALL PASSED (n checks)`.

MSVC example:

```bat
cl /nologo /W4 /std:c11 /Iinclude test\test_elib_drv.c ^
   src\elib_drv_manager.c src\elib_drv_tree.c src\elib_drv_node.c ^
   /Fe:t_drv.exe
t_drv.exe
```

## Concurrency Test

POSIX pthread environment:

```bash
gcc -std=c99 -pthread -Iinclude test/test_elib_drv_mt.c src/*.c \
    -o t_mt
./t_mt
```

Coverage:

- Device fops forwarding `elib_drv_write(controller)` and direct controller
  writes share the controller node lock, so hardware access never interleaves.
- Eight concurrent opens of an EXCLUSIVE node: exactly one succeeds.

The file depends on `<pthread.h>` and is not part of the ARM default build. On a
Windows MSVC host without pthread it is reviewed but not part of the default
verification.

## Test Coverage

`test/test_elib_drv.c` covers:

- `dt_node_count`: empty tree, nested count, NULL children, depth overflow, NULL
  arguments
- init: automatic controller-first, shared controller, nodes without init,
  failure rollback, idempotent re-init
- tree validation: duplicate/empty names, missing/empty/self controller,
  insufficient capacity, bad port
- I/O: read/write without open, open/close refcount, exclusive, missing fops,
  argument validation, result greater than len
- controller forwarding: a device looks up its controller by name and calls the
  same unified I/O
- logging: LF/CRLF/NONE, persistence across init/deinit, invalid line ending
- port: lock create/lock/unlock counts, create failure, lock failure

## Artifacts

Keep `t_drv`, `t_drv_nolog`, `t_mt`, `*.o`, `*.obj` and `*.exe` under `build/`
or ignore them; do not commit build artifacts.
