/* elib_drv_internal.h - private to the implementation, never installed */
#ifndef ELIB_DRV_INTERNAL_H
#define ELIB_DRV_INTERNAL_H

#include "elib_drv.h"

/* Per-node init state stored in elib_drv_node_t.init_state. */
#define ELIB_DRV_NODE_INIT_NONE        0u
#define ELIB_DRV_NODE_INIT_IN_PROGRESS 1u
#define ELIB_DRV_NODE_INIT_DONE        2u

/* Single manager instance: the library has no global state besides this. */
typedef struct {
    uint8_t initialized;
    uint8_t initializing;          /* fops->init is running */
    const elib_drv_port_t *port;
    elib_drv_log_fn log;
    const char *log_eol;           /* "", "\n" or "\r\n" */

    const elib_drv_dt_t *tree;
    elib_drv_node_t *nodes;
    unsigned node_count;
    uint32_t init_seq;             /* order assigned to successfully initialized nodes */
} elib_drv_manager_t;

extern elib_drv_manager_t elib_drv_g_manager;

/* Mutex helpers: a NULL lock or a NULL port means "do not lock". A failing
 * port call is reported as -ELIB_DRV_ERR_LOCK. */
int  elib_drv_lock_helper(void *lock, uint32_t timeout_ms);
void elib_drv_unlock_helper(void *lock);

/* Internal tree helpers shared by manager/node code. */
int elib_drv_tree_count(const elib_drv_dt_t *tree, unsigned *out_count);
const elib_drv_dt_node_t *elib_drv_tree_find_dt(const elib_drv_dt_t *tree,
                                                const char *name);
elib_drv_node_t *elib_drv_node_from_dt(const elib_drv_dt_node_t *dt);

/* fmt must be a string literal and ELIB_DRV_LOG needs at least one variadic
 * argument. Messages without arguments use ELIB_DRV_LOG_PLAIN: both append
 * the configured line ending through "%s", so the callback receives one
 * complete line per record. */
#if ELIB_DRV_LOG_ENABLE
#define ELIB_DRV_LOG(fmt, ...)                                         \
    do {                                                               \
        if (elib_drv_g_manager.log != NULL) {                          \
            (void)elib_drv_g_manager.log(                              \
                fmt "%s", __VA_ARGS__,                                 \
                elib_drv_g_manager.log_eol != NULL                     \
                    ? elib_drv_g_manager.log_eol : "");                \
        }                                                              \
    } while (0)

#define ELIB_DRV_LOG_PLAIN(fmt)                                        \
    do {                                                               \
        if (elib_drv_g_manager.log != NULL) {                          \
            (void)elib_drv_g_manager.log(                              \
                fmt "%s",                                              \
                elib_drv_g_manager.log_eol != NULL                     \
                    ? elib_drv_g_manager.log_eol : "");                \
        }                                                              \
    } while (0)
#else
#define ELIB_DRV_LOG(fmt, ...) ((void)0)
#define ELIB_DRV_LOG_PLAIN(fmt) ((void)0)
#endif

#endif /* ELIB_DRV_INTERNAL_H */
