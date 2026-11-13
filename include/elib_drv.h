/* elib_drv.h - lightweight MCU driver framework centered on a device tree
 *
 * Design rules:
 *  - The device tree is the only configuration source. Every tree node has a
 *    unique name, one fops table, one opaque void *ctx, an optional controller
 *    node name and optional children. Nodes do not have controller/device
 *    classes: a node with children can act as a controller, a node referring
 *    to another node is a device, and every node uses the same I/O API.
 *  - elib_drv_init_tree() walks the tree, fills user supplied node handles,
 *    creates one lock per node and calls fops->init automatically. A node's
 *    controller is initialized before the node itself.
 *  - Zero dynamic memory: the library never allocates. The caller owns the
 *    tree, the node handle array, fops tables, ctx objects and RTOS mutexes.
 *  - Unified I/O: open/close/read/write/ioctl address any node. A device
 *    driver obtains its controller with elib_drv_node_controller_name() and
 *    elib_drv_node_find(), then calls the same elib_drv_read/write/ioctl()
 *    on the controller node. Controller-specific target selection belongs to
 *    the controller driver.
 *  - Optional printf style flow log, called outside the framework locks.
 */
#ifndef ELIB_DRV_H
#define ELIB_DRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELIB_DRV_VERSION "2.0.0"

/* passed through to the port unchanged: wait for the mutex forever */
#define ELIB_DRV_WAIT_FOREVER  UINT32_MAX

#define ELIB_DRV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Maximum tree nesting levels (root children are level 1). This is a depth
 * limit, NOT a limit on the number of children/device nodes: a controller may
 * have as many child devices as the handle array can hold. */
#define ELIB_DRV_DT_MAX_DEPTH 8

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
 * ignores the return value. It may be called from several tasks concurrently,
 * so the callback must be thread safe. It must not call any elib_drv_*
 * function. NULL disables logging.
 *
 * elib_drv_set_log() also selects the line ending appended to every record:
 * 0 = none, 1 = '\n' (recommended default), 2 = "\r\n". The callback receives
 * the complete line. The setting is kept across init/deinit. */
typedef int (*elib_drv_log_fn)(const char *fmt, ...);

#define ELIB_DRV_LOG_NEWLINE_NONE  0u
#define ELIB_DRV_LOG_NEWLINE_LF    1u
#define ELIB_DRV_LOG_NEWLINE_CRLF  2u

int elib_drv_set_log(elib_drv_log_fn fn, unsigned newline);

/* ---- unified node fops -------------------------------------------------- */

struct elib_drv_node;
typedef struct elib_drv_node elib_drv_node_t;

typedef struct {
    int (*init)(elib_drv_node_t *node);                   /* optional */
    int (*deinit)(elib_drv_node_t *node);                 /* optional */
    int (*open)(elib_drv_node_t *node, uint32_t flags);   /* optional */
    int (*close)(elib_drv_node_t *node);                  /* optional */
    int (*read)(elib_drv_node_t *node, void *buf, size_t len);
    int (*write)(elib_drv_node_t *node, const void *buf, size_t len);
    int (*ioctl)(elib_drv_node_t *node, uint32_t cmd, void *arg);
} elib_drv_fops_t;

/* ---- device tree (const data, normally in flash) -----------------------
 *
 * controller: optional controller node name. If NULL and the node is a child
 * of another node, that parent is the controller. A root node without a
 * controller name is unbound.
 * ctx: the only per-node user pointer. Hardware configuration and driver
 * state both live behind it; the driver casts it to its own type.
 * children: any node may have children; a node acting as a controller should
 * list its devices here. Depth is limited by ELIB_DRV_DT_MAX_DEPTH. */
typedef struct elib_drv_dt_node {
    const char    *name;          /* globally unique, required */
    const elib_drv_fops_t *fops;  /* optional */
    void          *ctx;           /* hardware config/state, driver casts */
    const char    *controller;    /* optional controller node name */
    uint32_t       flags;         /* ELIB_DRV_DEV_F_* */
    uint32_t       lock_timeout_ms;
    const struct elib_drv_dt_node *children;
    unsigned       child_count;
} elib_drv_dt_node_t;

typedef struct {
    const elib_drv_dt_node_t *children;   /* root nodes */
    unsigned                  child_count;
} elib_drv_dt_t;

/* ---- runtime node handle ------------------------------------------------
 *
 * One handle per tree node. The caller supplies an array of handles to
 * elib_drv_init_tree(); the framework fills it. All fields are maintained by
 * the framework and must be treated as read only. */
struct elib_drv_node {
    const elib_drv_dt_node_t *dt;
    elib_drv_node_t          *controller;   /* resolved controller, or NULL */
    void                     *lock;
    uint32_t                  child_count;
    uint32_t                  open_count;
    uint32_t                  init_order;   /* 1..N after successful init */
    uint8_t                   init_state;   /* framework internal */
};

/* ---- RTOS port: the only piece that needs porting ----------------------
 *
 * A NULL port disables locking (bare metal, single threaded). When a port is
 * given all four callbacks are required. mutex_lock returns 0 on success and
 * a negative value on timeout/failure; the framework maps that to
 * -ELIB_DRV_ERR_LOCK. The framework never allocates, so the port owns the
 * mutex storage. The port object and every node lock must outlive the manager.
 */
typedef struct {
    void *(*mutex_create)(const char *name);
    void  (*mutex_destroy)(void *mutex);
    int   (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    int   (*mutex_unlock)(void *mutex);
} elib_drv_port_t;

#define ELIB_DRV_DEV_F_EXCLUSIVE (1u << 0)
#define ELIB_DRV_IOC(group, nr) \
    ((((uint32_t)(group) & 0xFFu) << 8) | ((uint32_t)(nr) & 0xFFu))

/* ---- manager ----------------------------------------------------------- */

/* Count all nodes of a tree. Can be called before init, for example to size
 * the handle array. Returns 0 and sets *out_count on success. An empty tree
 * yields 0. Returns -INVALID_PARAM for NULL arguments, inconsistent
 * children pointers/counts, or a tree deeper than ELIB_DRV_DT_MAX_DEPTH. */
int elib_drv_dt_node_count(const elib_drv_dt_t *tree, unsigned *out_count);

/* Initialize from the tree. nodes/node_capacity is the caller supplied handle
 * array; capacity must be >= the tree node count (see dt_node_count()).
 * Every node is initialized automatically, controller before dependent node.
 * fops->init may be called again after a deinit/re-init cycle, so fops must be
 * idempotent. */
int elib_drv_init_tree(const elib_drv_port_t *port,
                       const elib_drv_dt_t *tree,
                       elib_drv_node_t *nodes,
                       unsigned node_capacity);
int elib_drv_deinit(void);

/* Find by node name. Returns NULL when not initialized or not found. */
elib_drv_node_t *elib_drv_node_find(const char *name);

/* Walk every node in tree order. A non-zero return from fn stops the walk and
 * is returned as is. fn must not init/deinit the manager. */
int elib_drv_node_foreach(int (*fn)(elib_drv_node_t *node, void *arg),
                          void *arg);

/* ---- unified node I/O ---------------------------------------------------
 *
 * read/write/ioctl work on any initialized node, with or without open(). A
 * device driver may therefore find its controller and call elib_drv_read() on
 * it directly. open/close are optional per node and only invoke fops->open/
 * fops->close plus the EXCLUSIVE flag handling.
 *
 * read/write require buf != NULL when len != 0; a driver result larger than
 * len is mapped to -IO. Missing fops hooks return -NOT_SUPPORTED. */
int elib_drv_open(elib_drv_node_t *node, uint32_t flags);
int elib_drv_close(elib_drv_node_t *node);
int elib_drv_read(elib_drv_node_t *node, void *buf, size_t len);
int elib_drv_write(elib_drv_node_t *node, const void *buf, size_t len);
int elib_drv_ioctl(elib_drv_node_t *node, uint32_t cmd, void *arg);

/* Explicit node lock, for drivers that need to group several operations on a
 * controller. The same lock is taken by framework I/O. */
int elib_drv_node_lock(elib_drv_node_t *node, uint32_t timeout_ms);
int elib_drv_node_unlock(elib_drv_node_t *node);

/* ---- accessors ---------------------------------------------------------- */

const char *elib_drv_node_name(const elib_drv_node_t *node);
void       *elib_drv_node_ctx(const elib_drv_node_t *node);
elib_drv_node_t *elib_drv_node_controller(const elib_drv_node_t *node);
const char *elib_drv_node_controller_name(const elib_drv_node_t *node);
unsigned    elib_drv_node_open_count(const elib_drv_node_t *node);
unsigned    elib_drv_node_child_count(const elib_drv_node_t *node);

/* ---- of style tree queries ---------------------------------------------
 *
 * elib_drv_of_find() requires an initialized manager and returns the const
 * tree node, not the runtime handle. The *_config functions are NULL safe. */
const elib_drv_dt_node_t *elib_drv_of_find(const char *name);
const void *elib_drv_of_config(const elib_drv_dt_node_t *dt);
const char *elib_drv_of_name(const elib_drv_dt_node_t *dt);
const char *elib_drv_of_controller(const elib_drv_dt_node_t *dt);
const elib_drv_dt_node_t *elib_drv_of_children(
    const elib_drv_dt_node_t *dt, unsigned *child_count);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_DRV_H */
