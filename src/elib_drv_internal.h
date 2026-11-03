/* elib_drv_internal.h - private to the implementation, never installed */
#ifndef ELIB_DRV_INTERNAL_H
#define ELIB_DRV_INTERNAL_H

#include "elib_drv.h"

/* Single manager instance: the library has no global state besides this. */
typedef struct {
    uint8_t initialized;
    const elib_drv_port_t *port;
    elib_drv_log_fn log;

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
int  elib_drv__lock(void *lock, uint32_t timeout_ms);
void elib_drv__unlock(void *lock);

#if ELIB_DRV_LOG_ENABLE
#define ELIB_DRV_LOG(...)                                              \
    do {                                                               \
        if (elib_drv_g_manager.log != NULL) {                          \
            (void)elib_drv_g_manager.log(__VA_ARGS__);                 \
        }                                                              \
    } while (0)
#else
#define ELIB_DRV_LOG(...) ((void)0)
#endif

#endif /* ELIB_DRV_INTERNAL_H */
