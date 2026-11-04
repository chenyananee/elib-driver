/* elib_drv_internal.h - private to the implementation, never installed */
#ifndef ELIB_DRV_INTERNAL_H
#define ELIB_DRV_INTERNAL_H

#include "elib_drv.h"

/* Single manager instance: the library has no global state besides this. */
typedef struct {
    uint8_t initialized;
    uint8_t initializing;          /* controller ops->init is running */
    uint32_t epoch_counter;        /* monotonic registration epoch */
    uint32_t active_epoch;         /* epoch attached to new devices */
    elib_drv_controller_t *initializing_ctrl; /* controller whose init runs */
    const elib_drv_port_t *port;
    elib_drv_log_fn log;
    const char *log_eol;           /* "", "\n" or "\r\n" */

    const elib_drv_controller_desc_t *controllers;
    unsigned controller_count;
    const elib_drv_device_desc_t *devices;
    unsigned device_count;

    elib_drv_controller_slot_t *ctrl_slots;
    unsigned ctrl_capacity;
    elib_drv_device_slot_t *dev_slots;
    unsigned dev_capacity;
} elib_drv_manager_t;

extern elib_drv_manager_t elib_drv_g_manager;

/* Mutex helpers: a NULL lock or a NULL port means "do not lock". A failing
 * port call is reported as -ELIB_DRV_ERR_LOCK. */
int  elib_drv_lock_helper(void *lock, uint32_t timeout_ms);
void elib_drv_unlock_helper(void *lock);

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
