/* elib_drv.h - lightweight MCU driver framework (manager/controller/device)
 *
 * Design rules:
 *  - Linux file_operations flavour: every device exposes open/close/read/
 *    write/ioctl, and the framework owns validation, state and locking; a
 *    controller can also be opened and used directly (UART, bare ADC, ...)
 *    through its fops table, bypassing the device layer
 *  - static configuration is read-only: descriptors live in flash, and the
 *    user declares one small runtime context per controller and device
 *  - optional runtime table: hand the manager a slot cache and controllers
 *    and devices can be registered/unregistered at run time; the capacity is
 *    derived from the byte count, no compile-time maximum exists
 *  - zero dynamic memory: the library never allocates; RTOS mutexes come from
 *    a user supplied port (NULL port = bare metal, no locking at all)
 *  - optional printf style flow log, registered by the user; read/write/ioctl
 *    are never logged and the framework always logs outside its locks
 */
#ifndef ELIB_DRV_H
#define ELIB_DRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELIB_DRV_VERSION "0.2.0"

/* passed through to the port unchanged: wait for the mutex forever */
#define ELIB_DRV_WAIT_FOREVER  UINT32_MAX

#define ELIB_DRV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* compile the flow log in (default) or out; -DELIB_DRV_LOG_ENABLE=0 strips
 * every log call and format string, the runtime API stays available */
#ifndef ELIB_DRV_LOG_ENABLE
#define ELIB_DRV_LOG_ENABLE 1
#endif

/* ---- error codes --------------------------------------------------------
 *
 * Every function returns int: 0 or a positive value on success (> 0 means
 * bytes for read/write, driver defined for ioctl), a negative value on
 * failure, where the value is always -(one of the codes below). Driver
 * private errors start at -ELIB_DRV_ERR_DRIVER_BASE - n. */
typedef enum {
    ELIB_DRV_OK = 0,
    ELIB_DRV_ERR_INVALID_PARAM,
    ELIB_DRV_ERR_NOT_INITIALIZED,
    ELIB_DRV_ERR_ALREADY_INIT,
    ELIB_DRV_ERR_ALREADY_EXIST,
    ELIB_DRV_ERR_NOT_FOUND,
    ELIB_DRV_ERR_NO_SLOT,
    ELIB_DRV_ERR_BUSY,
    ELIB_DRV_ERR_NOT_OPEN,
    ELIB_DRV_ERR_NOT_SUPPORTED,
    ELIB_DRV_ERR_LOCK,
    ELIB_DRV_ERR_TIMEOUT,
    ELIB_DRV_ERR_IO,
    ELIB_DRV_ERR_DRIVER_BASE = 0x100
} elib_drv_err_t;

/* ---- optional flow log --------------------------------------------------
 *
 * A printf style callback: register printf itself, a UART logger, RTT, ...
 * The framework calls it outside its locks, only when it is not NULL, and
 * ignores the return value. It may be called from several tasks concurrently
 * (different devices), so the callback must be thread safe. It must not call
 * any elib_drv_* function. NULL disables logging. */
typedef int (*elib_drv_log_fn)(const char *fmt, ...);

int elib_drv_set_log(elib_drv_log_fn fn);

/* ---- handles and runtime contexts -------------------------------------- */

struct elib_drv_controller_desc;
struct elib_drv_device_desc;
typedef struct elib_drv_controller_desc elib_drv_controller_desc_t;
typedef struct elib_drv_device_desc     elib_drv_device_desc_t;

/* Per controller runtime state. The user declares one object per entry and
 * points the descriptor at it; a runtime table slot embeds its own. Fields
 * are maintained by the framework and must never be written by the user. */
typedef struct {
    const struct elib_drv_controller_desc *desc;
    void    *lock;
    uint32_t device_count;          /* devices bound to this controller */
    uint32_t open_count;            /* direct open/close reference count */
} elib_drv_controller_t;

/* Per device runtime state, same rules as above. */
typedef struct {
    const struct elib_drv_device_desc *desc;
    void     *lock;
    elib_drv_controller_t *controller;
    uint32_t open_count;
    uint32_t reserved;              /* registration epoch, framework internal */
} elib_drv_device_t;

/* ---- controller ops (const, flash) -------------------------------------
 *
 * Both hooks are optional. init is called after the contexts are wired, so it
 * may look up and register its own devices. A failing init (negative return)
 * must close direct opens it made on OTHER controllers and clean up any state
 * outside the framework. The framework does NOT call deinit for that
 * controller; it releases every runtime device registered while this init ran
 * and destroys the controller lock. deinit therefore runs only after a
 * successful init. */

typedef struct {
    int (*init)(elib_drv_controller_t *ctrl);    /* optional */
    int (*deinit)(elib_drv_controller_t *ctrl);  /* optional */
} elib_drv_controller_ops_t;

/* ---- controller direct access ops (const, flash) -----------------------
 *
 * A controller such as a UART may be used directly by the application, with
 * no device layer in between. These hooks are optional; a NULL table (or a
 * NULL hook) simply disables that direct operation. The framework calls them
 * with the controller lock held, so they must not lock it again and must not
 * call elib_drv_device_* (that would invert the device -> controller order). */

typedef struct {
    int (*open)(elib_drv_controller_t *ctrl, uint32_t flags);
    int (*close)(elib_drv_controller_t *ctrl);
    int (*read)(elib_drv_controller_t *ctrl, void *buf, size_t len);
    int (*write)(elib_drv_controller_t *ctrl, const void *buf, size_t len);
    int (*ioctl)(elib_drv_controller_t *ctrl, uint32_t cmd, void *arg);
} elib_drv_controller_fops_t;

/* ---- device ops: file_operations flavour ------------------------------- */

typedef struct {
    int (*open)(elib_drv_device_t *dev, uint32_t flags);
    int (*close)(elib_drv_device_t *dev);
    int (*read)(elib_drv_device_t *dev, void *buf, size_t len);
    int (*write)(elib_drv_device_t *dev, const void *buf, size_t len);
    int (*ioctl)(elib_drv_device_t *dev, uint32_t cmd, void *arg);
} elib_drv_ops_t;

#define ELIB_DRV_DEV_F_EXCLUSIVE (1u << 0)
#define ELIB_DRV_IOC(group, nr) \
    ((((uint32_t)(group) & 0xFFu) << 8) | ((uint32_t)(nr) & 0xFFu))

/* ---- descriptors (const table in flash; runtime table keeps a copy) ---- */

struct elib_drv_controller_desc {
    const char *name;                       /* stable for the whole lifetime */
    const elib_drv_controller_ops_t *ops;   /* lifecycle, may be NULL */
    const elib_drv_controller_fops_t *fops; /* direct access, may be NULL */
    void *priv;                             /* driver data, untouched */
    elib_drv_controller_t *ctx;             /* static table: required */
    uint32_t lock_timeout_ms;               /* direct calls, 0 = forever */
};

struct elib_drv_device_desc {
    const char *name;                       /* stable until unregistered */
    const elib_drv_ops_t *ops;              /* may be NULL */
    void *priv;                             /* driver data, untouched */
    elib_drv_device_t *ctx;                 /* static table: required */
    const char *controller;                 /* controller name, NULL = none */
    uint16_t flags;                         /* ELIB_DRV_DEV_F_* */
    uint16_t reserved;
    uint32_t lock_timeout_ms;               /* 0 = ELIB_DRV_WAIT_FOREVER */
};

/* ---- optional runtime table --------------------------------------------
 *
 * One slot is the same descriptor plus the context it drives. The user hands
 * the manager an array of slots and its byte count; the capacity is
 * bytes / sizeof(slot), rounded down. Without a runtime table every register
 * call fails with -ELIB_DRV_ERR_NO_SLOT. */
typedef struct {
    elib_drv_controller_desc_t desc;
    elib_drv_controller_t      ctx;
} elib_drv_controller_slot_t;

typedef struct {
    elib_drv_device_desc_t desc;
    elib_drv_device_t      ctx;
} elib_drv_device_slot_t;

typedef struct {
    void  *mem;
    size_t bytes;
} elib_drv_mem_t;

typedef struct {
    elib_drv_mem_t controllers;
    elib_drv_mem_t devices;
} elib_drv_runtime_t;

typedef struct {
    const elib_drv_controller_desc_t *controllers;
    unsigned controller_count;
    const elib_drv_device_desc_t *devices;
    unsigned device_count;
    const elib_drv_runtime_t *runtime;   /* NULL = static table only */
} elib_drv_config_t;

/* ---- RTOS port: the only piece that needs porting ----------------------
 *
 * A NULL port disables locking (bare metal, single threaded). When a port is
 * given all four callbacks are required. mutex_lock returns 0 on success and
 * a negative value on timeout/failure; the framework maps that to
 * -ELIB_DRV_ERR_LOCK. The framework never allocates, so the port owns the
 * mutex storage (heap or a static pool). The struct itself must outlive the
 * manager (store it statically); the descriptor tables and runtime cache do
 * so as well. */
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;

/* ---- manager ----------------------------------------------------------- */

int elib_drv_init(const elib_drv_port_t *port, const elib_drv_config_t *config);
int elib_drv_deinit(void);

elib_drv_controller_t *elib_drv_controller_find(const char *name);
elib_drv_device_t     *elib_drv_device_find(const char *name);

/* Front to back: static devices first, then runtime slots. A non zero return
 * value from fn stops the walk and is returned as is. fn must not register or
 * unregister anything. */
int elib_drv_device_foreach(int (*fn)(elib_drv_device_t *dev, void *arg),
                            void *arg);

/* ---- runtime registration ----------------------------------------------
 *
 * Registration copies the descriptor into a free slot of the runtime table;
 * out (may be NULL) receives the new handle. Unregistering a static entry
 * returns -ELIB_DRV_ERR_NOT_SUPPORTED. A runtime handle is invalid as soon as
 * it is unregistered: unregistering it again returns
 * -ELIB_DRV_ERR_INVALID_PARAM and an old handle must never be used after its
 * slot has been reused. A controller that calls unregister on itself from its
 * own ops->init gets -ELIB_DRV_ERR_BUSY. */

int elib_drv_register_controller(const elib_drv_controller_desc_t *desc,
                                 elib_drv_controller_t **out);
int elib_drv_unregister_controller(elib_drv_controller_t *ctrl);
int elib_drv_register_device(const elib_drv_device_desc_t *desc,
                             elib_drv_device_t **out);
int elib_drv_unregister_device(elib_drv_device_t *dev);

/* ---- user API: file_operations flavour --------------------------------- */

int elib_drv_open(elib_drv_device_t *dev, uint32_t flags);
int elib_drv_close(elib_drv_device_t *dev);
int elib_drv_read(elib_drv_device_t *dev, void *buf, size_t len);
int elib_drv_write(elib_drv_device_t *dev, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_device_t *dev, uint32_t cmd, void *arg);

/* ---- controller direct access (bypasses the device layer) --------------
 *
 * Same file_operations flavour as the device API, but addressed at the
 * controller itself (a UART, a bare ADC, ...). The framework serialises every
 * call through the controller lock, the same lock device drivers take for a
 * shared bus, so direct users and devices never interleave a transfer. Direct
 * opens are reference counted and independent from device opens: deinit and
 * unregister_controller return -ELIB_DRV_ERR_BUSY while a direct open is
 * still pending. */

int elib_drv_controller_open(elib_drv_controller_t *ctrl, uint32_t flags);
int elib_drv_controller_close(elib_drv_controller_t *ctrl);
int elib_drv_controller_read(elib_drv_controller_t *ctrl, void *buf,
                             size_t len);
int elib_drv_controller_write(elib_drv_controller_t *ctrl, const void *buf,
                              size_t len);
int elib_drv_controller_ioctl(elib_drv_controller_t *ctrl, uint32_t cmd,
                              void *arg);

/* ---- driver side accessors / controller lock --------------------------- */

const char *elib_drv_controller_name(const elib_drv_controller_t *ctrl);
void       *elib_drv_controller_priv(const elib_drv_controller_t *ctrl);
unsigned    elib_drv_controller_open_count(const elib_drv_controller_t *ctrl);
unsigned    elib_drv_controller_device_count(const elib_drv_controller_t *ctrl);
int         elib_drv_controller_lock(elib_drv_controller_t *ctrl,
                                     uint32_t timeout_ms);
int         elib_drv_controller_unlock(elib_drv_controller_t *ctrl);

const char *elib_drv_device_name(const elib_drv_device_t *dev);
void       *elib_drv_device_priv(const elib_drv_device_t *dev);
elib_drv_controller_t *elib_drv_device_controller(const elib_drv_device_t *dev);
unsigned    elib_drv_device_open_count(const elib_drv_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_DRV_H */
