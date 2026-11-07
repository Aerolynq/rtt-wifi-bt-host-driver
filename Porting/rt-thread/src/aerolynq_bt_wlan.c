/***********************************************************************************************
 * @file aerolynq_bt_wlan.c
 *
 * \copyright
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 *                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
 * Website: https://www.aerolynq.com
 *
 * SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
 *
 * PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
 *
 * This file is the proprietary work of Aerolynq and is NOT released
 * under the Apache License, MIT, BSD or any other open-source license.
 * Use is permitted EXCLUSIVELY with Aerolynq WiFi / Bluetooth combo
 * modules. Use with any other hardware is strictly prohibited and
 * constitutes copyright infringement. See the NOTICE and LICENSE
 * files at the root of this package for the complete terms.
 *
 * Change Logs:
 * Date         Author      Notes
 * 2025-12-18   Aerolynq
 * 2025-12-19   Aerolynq
 **************************************************************************************************/

#include <rtthread.h>
#include <rtdevice.h>
/* WLAN header compatibility for different RT-Thread versions */
#if (RT_VER_NUM >= 0x50200)
#include <drivers/wlan.h>
#else
#include <wlan_dev.h>
#include <wlan_prot.h>
#endif
#include <netif/ethernetif.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>

#include "cybsp.h"
#include "cyabs_rtos.h"
#include "cyhal_sdio.h"

/* Bluetooth includes */
#ifdef WHD_USING_BLUETOOTH
#include "wiced_bt_stack.h"
#include "wiced_memory.h"
#include "wiced_bt_dev.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_gatt.h"
#include "cybt_platform_config.h"
#include "cybt_platform_trace.h"
#include "ble_app.h"
#endif

#include "whd.h"
#include "whd_int.h"
#include "whd_wifi_api.h"
#include "whd_types.h"
#include "whd_wlioctl.h"
#include "whd_buffer_api.h"
#include "whd_resource_api.h"
#include "whd_network_types.h"
#include "wiced_resource.h"


#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "netdev.h"

/* RT_MIN macro if not defined */
#ifndef RT_MIN
#define RT_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define DBG_TAG           "whd.wlan"
#define DBG_LVL           DBG_INFO
#include "rtdbg.h"

/*******************************************************************************
 * File system mount hook
 ******************************************************************************/

/**
 * @brief Wait until the file system holding the WiFi firmware is mounted
 *
 * When the combined firmware is read from a file system (SD card, USB disk),
 * the firmware file must not be opened before the file system is mounted.
 *
 * The board provides a strong implementation of this function that blocks
 * until the mount has completed. The weak default below does nothing, so
 * that boards without a file system keep working unchanged.
 */
rt_weak void whd_wait_fs_mount(void)
{
}

/*******************************************************************************
 * Configuration Macros
 ******************************************************************************/

/* Default PM2 return-to-sleep delay (ms), same as WHD internal default */
#ifndef DEFAULT_PM2_SLEEP_RET_TIME
#define DEFAULT_PM2_SLEEP_RET_TIME          (200)
#endif

#ifndef CY_WIFI_WHD_THREAD_PRIORITY
#ifdef WBHD_WIFI_THREAD_PRIORITY
#define CY_WIFI_WHD_THREAD_PRIORITY         WBHD_WIFI_THREAD_PRIORITY
#else
#define CY_WIFI_WHD_THREAD_PRIORITY         (CY_RTOS_PRIORITY_HIGH)
#endif
#endif

#ifndef CY_WIFI_WHD_THREAD_STACK_SIZE
#ifdef WBHD_WIFI_THREAD_STACK_SIZE
#define CY_WIFI_WHD_THREAD_STACK_SIZE       WBHD_WIFI_THREAD_STACK_SIZE
#else
#define CY_WIFI_WHD_THREAD_STACK_SIZE       (5120)
#endif
#endif

#ifndef CY_WIFI_INIT_THREAD_STACK_SIZE
#define CY_WIFI_INIT_THREAD_STACK_SIZE      (4096)
#endif

/* Priority of the WiFi initialization thread.
 *
 * When the firmware is read from a file system, the initialization runs in
 * its own thread. It must have a lower priority than the main thread, so
 * that the main thread finishes the component initialization (which mounts
 * the file system) before the WiFi thread continues. */
#ifndef CY_WIFI_INIT_THREAD_PRIORITY
#ifdef WBHD_WIFI_INIT_THREAD_PRIORITY
#define CY_WIFI_INIT_THREAD_PRIORITY        WBHD_WIFI_INIT_THREAD_PRIORITY
#else
#define CY_WIFI_INIT_THREAD_PRIORITY        (20)
#endif
#endif

#define SCAN_TIMEOUT_MS                     (10000)
#define DHCP_TIMEOUT_MS                     (30000)

/* WiFi join retry configuration */
#ifndef WHD_JOIN_MAX_RETRIES
#define WHD_JOIN_MAX_RETRIES                (3)       /**< Max join retries on first-time connect */
#endif
#ifndef WHD_JOIN_RETRY_DELAY_MS
#define WHD_JOIN_RETRY_DELAY_MS             (200)     /**< Delay between join retries (ms) */
#endif

/* SDIO error detection - stop retry when SDIO communication fails */
#define WHD_SDIO_ERROR_MASK                 (0x060E0000)
#define IS_SDIO_ERROR(ret)                  (((ret) & WHD_SDIO_ERROR_MASK) == WHD_SDIO_ERROR_MASK)


#ifndef WHD_FAST_JOIN_SCAN_ENABLE
#define WHD_FAST_JOIN_SCAN_ENABLE          (1)
#endif

/* Number of probe requests sent per channel during the fast scan */
#ifndef WHD_FAST_SCAN_PROBES
#define WHD_FAST_SCAN_PROBES               (2)
#endif

/* Active dwell time per channel during the fast scan (ms) */
#ifndef WHD_FAST_SCAN_ACTIVE_DWELL_MS
#define WHD_FAST_SCAN_ACTIVE_DWELL_MS      (30)
#endif

/* Home channel dwell time between channels during the fast scan (ms).
 * 0 disables the extra home-channel dwell, which is what makes an
 * already-associated STA spend much longer per scan. */
#ifndef WHD_FAST_SCAN_HOME_DWELL_MS
#define WHD_FAST_SCAN_HOME_DWELL_MS        (0)
#endif

/* Passive dwell time per channel during the fast scan (ms). The fast scan
 * is active-only, this value is only kept for completeness. */
#ifndef WHD_FAST_SCAN_PASSIVE_DWELL_MS
#define WHD_FAST_SCAN_PASSIVE_DWELL_MS     (30)
#endif

/* Total time budget for the whole managed scan sequence (fast scan plus
 * optional fallback scan). The WLAN framework waits for the SCAN_DONE
 * event for RT_WLAN_CONNECT_WAIT_MS (10 s), so the driver must report it
 * within that window. The budget is a bit smaller to leave room for the
 * framework overhead. */
#ifndef WHD_FAST_SCAN_SEQUENCE_BUDGET_MS
#define WHD_FAST_SCAN_SEQUENCE_BUDGET_MS   (8000)
#endif

/* ------------------------------------------------------------------------
 * Early stop of the join-path scan
 * ------------------------------------------------------------------------
 *
 * The directed scan is aborted (WL_SCAN_ACTION_ABORT) once the target SSID
 * was seen and the grace period expired, so that a stronger BSS of the same
 * SSID can still win. The callback is still invoked with WHD_SCAN_ABORTED,
 * the event ordering stays the same.
 */
#ifndef WHD_FAST_SCAN_EARLY_STOP_ENABLE
#define WHD_FAST_SCAN_EARLY_STOP_ENABLE     (1)
#endif

/* Grace period after the first target match */
#ifndef WHD_FAST_SCAN_EARLY_STOP_GRACE_MS
#define WHD_FAST_SCAN_EARLY_STOP_GRACE_MS   (150)
#endif

/* Upper bound for the wait of the abort callback, so that a lost callback
 * cannot block the join. */
#ifndef WHD_FAST_SCAN_EARLY_STOP_ABORT_TIMEOUT_MS
#define WHD_FAST_SCAN_EARLY_STOP_ABORT_TIMEOUT_MS   (500)
#endif

/* ------------------------------------------------------------------------
 * Channel cache for the join-path scan
 * ------------------------------------------------------------------------
 *
 * The channel of an AP seen by a previous scan is remembered, so that the
 * next join-path scan can be restricted to that single channel instead of
 * sweeping the whole band. The cache is only a hint: if the AP is not found
 * on the cached channel, the scan is repeated over the whole band.
 */

/* Maximum age of a cached channel */
#ifndef WHD_SCAN_CHANNEL_CACHE_TTL_MS
#define WHD_SCAN_CHANNEL_CACHE_TTL_MS     (30000)
#endif

/* Maximum number of cached channels */
#ifndef WHD_SCAN_CHANNEL_CACHE_ENTRIES
#define WHD_SCAN_CHANNEL_CACHE_ENTRIES    (4)
#endif

/* Additional event definitions not in whd_events.h
 * Values must match whd_events_int.h. */
#ifndef WLC_E_CSA_COMPLETE_IND
#define WLC_E_CSA_COMPLETE_IND              80    /**< CSA complete (from whd_events_int.h) */
#endif

#ifndef WLC_E_AUTHORIZED
#define WLC_E_AUTHORIZED                    136   /**< STA authorized for data (from whd_events_int.h) */
#endif

#ifndef WLC_E_TKO
#define WLC_E_TKO                           151   /**< TCP Keep Alive Offload event (from whd_events_int.h) */
#endif

#ifndef WLC_E_LINK_BCN_LOSS
#define WLC_E_LINK_BCN_LOSS                 (1)   /**< Link down due to beacon loss */
#endif

/* Link event message flags */
#define WLC_EVENT_MSG_LINK                  (0x01)

/* Band definitions for whd_wifi_set_ioctl_value */
#ifndef WLC_SET_BAND
#define WLC_SET_BAND                        (108)
#endif

#ifndef WLC_BAND_AUTO
#define WLC_BAND_AUTO                       (0)
#define WLC_BAND_5G                         (1)
#define WLC_BAND_2G                         (2)
#define WLC_BAND_ALL                        (3)
#endif

/* PSK supplicant reason codes (from whd_events_int.h) */
#ifndef WLC_E_SUP_REASON_OFFSET
#define WLC_E_SUP_REASON_OFFSET             (256)
#endif

#ifndef WLC_E_SUP_OTHER
#define WLC_E_SUP_OTHER                     (0 + WLC_E_SUP_REASON_OFFSET)
#endif

#ifndef WLC_E_SUP_MSG3_TOO_MANY_IE
#define WLC_E_SUP_MSG3_TOO_MANY_IE          (6 + WLC_E_SUP_REASON_OFFSET)
#endif

/* Roaming IOVAR strings */
#define IOVAR_STR_ROAM_OFF                  "roam_off"
#define IOVAR_STR_ROAM_TRIGGER              "roam_trigger"
#define IOVAR_STR_ROAM_DELTA                "roam_delta"
#define IOVAR_STR_ROAM_SCAN_PERIOD          "roam_scan_period"

/* The WPA3 and enterprise security types do not exist in every RT-Thread
 * WLAN framework version, so the conversion helpers below use the numeric
 * values instead of the framework names. The values must match the
 * framework definitions in dev_wlan.h exactly, otherwise the same flag
 * resolves to different values in different translation units. */

/*******************************************************************************
 * Type Definitions
 ******************************************************************************/

/**
 * @brief WiFi connection state
 */
typedef enum
{
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTING,
    WIFI_STATE_AP_STARTING,
    WIFI_STATE_AP_RUNNING,
    WIFI_STATE_ERROR
} wifi_state_t;

/**
 * @brief Scan stage of the driver-managed join-path scan
 *
 * A fast scan with a short per-channel dwell time runs first and is
 * repeated with the firmware default dwell times when the target AP was not
 * found.
 */
typedef enum
{
    WHD_SCAN_STAGE_IDLE = 0,      /**< No scan running */
    WHD_SCAN_STAGE_FAST,          /**< Fast (short dwell) scan running */
    WHD_SCAN_STAGE_FALLBACK,      /**< Fast scan done, fallback required */
    WHD_SCAN_STAGE_FALLBACK_RUN,  /**< Fallback (default dwell) scan running */
    WHD_SCAN_STAGE_DONE           /**< Last scan of the sequence finished */
} whd_scan_stage_t;
/**
 * @brief Scan context structure
 */
typedef struct
{
    struct rt_wlan_device   *wlan;
    whd_scan_result_t       scan_result;
    whd_scan_type_t         scan_type;
    rt_sem_t                scan_complete_sem;
    rt_bool_t               is_scanning;
    rt_bool_t               stop_requested;
    rt_bool_t               run_passive;
    /* Set while the driver itself supervises the scan (SSID-filtered join
     * path scan). In that mode a second scan with the firmware default
     * parameters may follow, and the SCAN_DONE event is reported by the scan
     * callback of the last scan of the sequence. */
    rt_bool_t               managed;
    /* Stage of the driver-managed scan sequence. */
    whd_scan_stage_t        stage;
    /* Set when the target SSID of the current scan was seen. Used to decide
     * whether the fast scan has to be repeated with the default dwell times. */
    rt_bool_t               target_found;
    /* Set when the early stop was requested because the target SSID was seen
     * and the grace period expired. The abort is performed by the driver
     * thread, not by the scan callback, so that the firmware call is not made
     * from the WHD callback context. */
    rt_bool_t               early_stop_requested;
    /* Tick value at which the target SSID was first seen. */
    rt_tick_t               target_found_tick;
    /* SSID the current scan is looking for. Part of this context (and not a
     * pointer into a caller's stack) so that it stays valid for the whole
     * scan, even if a scan callback arrives late. */
    whd_ssid_t              target_ssid;
    rt_bool_t               has_target_ssid;
    /* Set once the SCAN_DONE event was reported for the current scan, so that
     * a late scan callback does not report it a second time. */
    rt_bool_t               done_reported;
} scan_context_t;

/**
 * @brief Connection parameters storage
 */
typedef struct
{
    whd_ssid_t              ssid;
    whd_mac_t               bssid;
    whd_security_t          security;
    uint8_t                 key[64];
    uint8_t                 key_len;
    uint8_t                 channel;
    rt_bool_t               is_valid;
} connect_params_t;

/**
 * @brief WiFi driver context structure
 */
typedef struct drv_wifi
{
    struct rt_wlan_device   *wlan;
    whd_interface_t         whd_itf;
    wifi_state_t            state;
    rt_bool_t               is_initialized;
    uint16_t                event_handler_index;
} drv_wifi_t;

/*******************************************************************************
 * External Declarations
 ******************************************************************************/

extern whd_resource_source_t resource_ops;
extern struct whd_buffer_funcs whd_buffer_ops;

/*******************************************************************************
 * Static Variables
 ******************************************************************************/

/* WHD driver handle */
static whd_driver_t whd_driver = NULL;

/* SDIO interface */
static cyhal_sdio_t cyhal_sdio;

/* WHD initialization configuration */
static whd_init_config_t whd_config = {
    .thread_priority    = CY_WIFI_WHD_THREAD_PRIORITY,
    .thread_stack_size  = CY_WIFI_WHD_THREAD_STACK_SIZE,
    .country            = CY_WIFI_COUNTRY,
};

/* WiFi interface instances */
static drv_wifi_t wifi_sta = {0};
static drv_wifi_t wifi_ap = {0};

/* Scan context */
static scan_context_t scan_ctx = {0};

/* Connected AP details for reconnection */
static connect_params_t connected_ap = {0};

/* Channel cache entry, see the description at WHD_SCAN_CHANNEL_CACHE_TTL_MS */
typedef struct
{
    rt_bool_t   valid;
    whd_ssid_t  ssid;
    uint8_t     channel;
    rt_tick_t   age_ticks;
} scan_channel_cache_t;

static scan_channel_cache_t scan_channel_cache_tab[WHD_SCAN_CHANNEL_CACHE_ENTRIES];

/* Mutex for thread safety */
static rt_mutex_t wifi_mutex = RT_NULL;
static rt_bool_t mutex_initialized = RT_FALSE;

/* Handshake state */
static rt_bool_t link_up_event_received = RT_FALSE;
/* Track disconnect state. The flag is recorded here but not read by the
 * connection logic, so it is silenced with RT_UNUSED where it is set. */
static rt_bool_t is_disconnect_triggered = RT_FALSE;

/* Current security type for handshake handling */
static whd_security_t sta_security_type = WHD_SECURITY_UNKNOWN;

/* Link state */
static rt_bool_t sta_link_up = RT_FALSE;
static rt_bool_t ap_link_up = RT_FALSE;
static rt_bool_t sta_roam_forced_off_by_ap = RT_FALSE;

#ifdef WHD_USING_WPA3
#include "cy_wcm.h"

/**
 * @brief WPA3 SAE External supplicant start (weak function)
 *
 * Overridden by the real implementation from External/wpa3-external-supplicant
 * when WHD_USING_WPA3 is enabled in Kconfig.
 */
__attribute__((weak)) cy_rslt_t wpa3_supplicant_sae_start(uint8_t *ssid, uint8_t ssid_len,
                                                           uint8_t *passphrase, uint8_t passphrase_len)
{
    LOG_W("WPA3 SAE: supplicant stub called - real implementation not linked");
    return CY_RSLT_SUCCESS;
}


/* Simple slot table for WCM event callbacks (WPA3 only registers one) */
#define WCM_SHIM_MAX_CALLBACKS  2
static cy_wcm_event_callback_t wcm_shim_callbacks[WCM_SHIM_MAX_CALLBACKS] = {0};

/**
 * @brief Get WHD interface handle for a given WCM interface type.
 *        Maps CY_WCM_INTERFACE_TYPE_STA → wifi_sta.whd_itf,
 *             CY_WCM_INTERFACE_TYPE_AP  → wifi_ap.whd_itf.
 */
cy_rslt_t cy_wcm_get_whd_interface(cy_wcm_interface_t interface_type, whd_interface_t *whd_iface)
{
    if (whd_iface == NULL)
        return CY_RSLT_MW_ERROR;

    switch (interface_type)
    {
        case CY_WCM_INTERFACE_TYPE_STA:
        case CY_WCM_INTERFACE_TYPE_AP_STA:
            *whd_iface = wifi_sta.whd_itf;
            break;
        case CY_WCM_INTERFACE_TYPE_AP:
            *whd_iface = wifi_ap.whd_itf;
            break;
        default:
            return CY_RSLT_MW_ERROR;
    }

    return (*whd_iface != NULL) ? CY_RSLT_SUCCESS : CY_RSLT_MW_ERROR;
}

/**
 * @brief Register a WCM event callback (used by WPA3 supplicant for cleanup notifications).
 */
cy_rslt_t cy_wcm_register_event_callback(cy_wcm_event_callback_t event_callback)
{
    if (event_callback == NULL)
        return CY_RSLT_MW_ERROR;

    for (int i = 0; i < WCM_SHIM_MAX_CALLBACKS; i++)
    {
        if (wcm_shim_callbacks[i] == NULL)
        {
            wcm_shim_callbacks[i] = event_callback;
            return CY_RSLT_SUCCESS;
        }
    }
    return CY_RSLT_MW_ERROR;  /* No free slot */
}

/**
 * @brief Deregister a WCM event callback.
 */
cy_rslt_t cy_wcm_deregister_event_callback(cy_wcm_event_callback_t event_callback)
{
    if (event_callback == NULL)
        return CY_RSLT_MW_ERROR;

    for (int i = 0; i < WCM_SHIM_MAX_CALLBACKS; i++)
    {
        if (wcm_shim_callbacks[i] == event_callback)
        {
            wcm_shim_callbacks[i] = NULL;
            return CY_RSLT_SUCCESS;
        }
    }
    return CY_RSLT_MW_ERROR;  /* Not found */
}

/**
 * @brief Fire a WCM event to all registered callbacks.
 *
 * The WPA3 supplicant tears its workspace down from its own 1 ms timer
 * (WPA3_SAE_HANDSHAKE_DELETE_MS), so this helper has no caller yet. It is
 * kept for the WCM-driven cleanup path (CY_WCM_EVENT_CONNECTED /
 * DISCONNECTED); the attribute below silences the "declared but never
 * referenced" warning.
 */
__attribute__((unused))
static void aerolynq_wcm_notify_event(cy_wcm_event_t event, cy_wcm_event_data_t *data)
{
    for (int i = 0; i < WCM_SHIM_MAX_CALLBACKS; i++)
    {
        if (wcm_shim_callbacks[i] != NULL)
        {
            wcm_shim_callbacks[i](event, data);
        }
    }
}

#endif /* WHD_USING_WPA3 */

/* Event list for STA link events.
 * WLC_E_ASSOC / WLC_E_REASSOC are intentionally omitted from the firmware
 * event mask during the join window to avoid spurious deauth events.
 */
static const uint32_t sta_link_events[] = {
    WLC_E_LINK,
    WLC_E_DEAUTH_IND,
    WLC_E_DISASSOC_IND,
    WLC_E_PSK_SUP,
    WLC_E_CSA_COMPLETE_IND,
    WLC_E_NONE
};

/* Roaming state */
static rt_bool_t roaming_enabled = RT_TRUE;

/* Event list for AP link events */
static const uint32_t ap_link_events[] = {
    WLC_E_DISASSOC_IND,
    WLC_E_DEAUTH_IND,
    WLC_E_ASSOC_IND,
    WLC_E_REASSOC_IND,
    WLC_E_AUTHORIZED,
    WLC_E_NONE
};

/* Current AP security type for event handling */
static whd_security_t ap_security_type = WHD_SECURITY_UNKNOWN;

/*******************************************************************************
 * Forward Declarations
 ******************************************************************************/

static void* sta_event_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                               const uint8_t *event_data, void *handler_user_data);
static void* ap_event_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                              const uint8_t *event_data, void *handler_user_data);
static void scan_callback(whd_scan_result_t **result_ptr, void *user_data, whd_scan_status_t status);
static cy_rslt_t wifi_connect_internal(drv_wifi_t *wifi, const connect_params_t *params);
static void link_up_handler(drv_wifi_t *wifi);
static void link_down_handler(drv_wifi_t *wifi, uint32_t reason);
static rt_bool_t security_requires_handshake(whd_security_t security);
static void scan_channel_cache_store(const whd_ssid_t *ssid, uint8_t channel);
static uint8_t scan_channel_cache_lookup(const whd_ssid_t *ssid);

/* Band constants (same as wlioctl.h) */
#ifndef WLC_BAND_AUTO
#define WLC_BAND_AUTO   0   /* auto-select */
#define WLC_BAND_5G     1   /* 5 GHz */
#define WLC_BAND_2G     2   /* 2.4 GHz */
#define WLC_BAND_ALL    3   /* all bands */
#endif

#ifdef WHD_USING_ROAM
/* Roaming API forward declarations */
rt_err_t whd_wlan_set_roaming(rt_bool_t enable);
rt_bool_t whd_wlan_get_roaming(void);
rt_err_t whd_wlan_set_roam_trigger(int32_t trigger_level, uint32_t band);
rt_err_t whd_wlan_get_roam_trigger(int32_t *trigger_level, uint32_t band);
rt_err_t whd_wlan_set_roam_delta(uint32_t delta, uint32_t band);
rt_err_t whd_wlan_get_roam_delta(uint32_t *delta, uint32_t band);
rt_err_t whd_wlan_set_roam_scan_period(uint32_t period);
rt_err_t whd_wlan_get_roam_scan_period(uint32_t *period);
rt_err_t whd_wlan_set_roam_time_threshold(uint32_t *threshold);
rt_err_t whd_wlan_get_roam_time_threshold(uint32_t *threshold);
rt_err_t whd_wlan_get_roam_trigger1(int32_t *trigger_level, uint32_t band);
rt_err_t whd_wlan_set_roam_trigger1(int32_t *trigger_level, uint32_t band);
#endif /* WHD_USING_ROAM */

/*******************************************************************************
 * Helper Functions
 ******************************************************************************/

/**
 * @brief Get driver context from wlan device
 */
static inline drv_wifi_t* get_drv_wifi(struct rt_wlan_device *wlan)
{
    if (wlan == wifi_sta.wlan)
        return &wifi_sta;
    if (wlan == wifi_ap.wlan)
        return &wifi_ap;
    return RT_NULL;
}

/**
 * @brief Lock WiFi mutex
 */
static inline rt_err_t wifi_lock(void)
{
    if (!mutex_initialized || wifi_mutex == RT_NULL)
        return -RT_ERROR;
    return rt_mutex_take(wifi_mutex, RT_WAITING_FOREVER);
}

/**
 * @brief Unlock WiFi mutex
 */
static inline void wifi_unlock(void)
{
    if (mutex_initialized && wifi_mutex != RT_NULL)
        rt_mutex_release(wifi_mutex);
}

static inline rt_bool_t mac_is_nonzero(const uint8_t *mac, uint32_t len)
{
    uint32_t i;

    if (mac == RT_NULL || len == 0)
        return RT_FALSE;

    for (i = 0; i < len; i++)
    {
        if (mac[i] != 0)
            return RT_TRUE;
    }

    return RT_FALSE;
}


/* The WHD security value is a bitmask with three groups of bits:
 *
 *   family   : WEP / WPA / WPA2 / WPA3 / enterprise / OWE
 *   cipher   : TKIP / AES
 *   modifier : SHA256_1X (PMF), FBT (802.11r), SUITE_B (WPA3 192-bit)
 *
 * The scan code ORs the bits of every advertised AKM and cipher suite
 * together, so the value reported for a BSS is a union of flags and never
 * matches a single whd_security_t entry. The modifier bits carry no
 * information about the security family and are masked out before the
 * family is determined.
 *
 * The values below are spelled out rather than taken from whd_types.h,
 * because the framework header dev_wlan.h defines macros with the same
 * names and the two headers may be included in a different order in
 * different translation units. The framework values must match these ones
 * exactly.
 */

/* Modifier bits */
#define AERO_WHD_FLAG_FBT              0x40000000
#define AERO_WHD_FLAG_SHA256_1X        0x04000000
#define AERO_WHD_FLAG_SUITE_B_SHA384   0x08000000

/* Family bits */
#define AERO_WHD_SEC_WEP                0x00000001
#define AERO_WHD_SEC_TKIP               0x00000002
#define AERO_WHD_SEC_AES                0x00000004
#define AERO_WHD_SEC_WPA                0x00200000
#define AERO_WHD_SEC_WPA2               0x00400000
#define AERO_WHD_SEC_WPA3               0x01000000
#define AERO_WHD_SEC_ENTERPRISE         0x02000000
#define AERO_WHD_FLAG_OWE               0x80000000

/* Framework security values. The numeric values are used instead of the
 * framework enum names, so that this file compiles whether or not the
 * framework provides the WPA3 and enterprise security types. */
#define AERO_RTT_SEC_WPA3_AES_PSK         0x01000004
#define AERO_RTT_SEC_WPA2_WPA3_PSK        0x01400004
#define AERO_RTT_SEC_WPA_ENTERPRISE       0x02200000
#define AERO_RTT_SEC_WPA2_ENTERPRISE      0x02400000
#define AERO_RTT_SEC_WPA3_ENTERPRISE      0x03000000
#define AERO_RTT_SEC_WPA2_WPA3_ENTERPRISE 0x03400000
#define AERO_RTT_SEC_WPA3_ENT_192         0x03000004
#define AERO_RTT_SEC_OWE                  0x00040000

static rt_wlan_security_t whd_to_rtt_security(whd_security_t whd_sec)
{
    rt_uint32_t sec = (rt_uint32_t)whd_sec;

    /* Exact matches first. These cover all the plain, unambiguous values
     * and preserve the behaviour of the existing code paths. */
    switch (sec)
    {
        case (rt_uint32_t)WHD_SECURITY_OPEN:
            return SECURITY_OPEN;

        case (rt_uint32_t)WHD_SECURITY_WEP_PSK:
        case (rt_uint32_t)WHD_SECURITY_WEP_SHARED:
            return SECURITY_WEP_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA_TKIP_PSK:
            return SECURITY_WPA_TKIP_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA_AES_PSK:
            return SECURITY_WPA_AES_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA_MIXED_PSK:
        case (rt_uint32_t)WHD_SECURITY_WPA2_WPA_AES_PSK:
        case (rt_uint32_t)WHD_SECURITY_WPA2_WPA_MIXED_PSK:
            return SECURITY_WPA2_MIXED_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA2_AES_PSK:
        case (rt_uint32_t)WHD_SECURITY_WPA2_AES_PSK_SHA256:
            return SECURITY_WPA2_AES_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA2_TKIP_PSK:
            return SECURITY_WPA2_TKIP_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA2_MIXED_PSK:
            return SECURITY_WPA2_MIXED_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA_TKIP_ENT:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA_ENTERPRISE;

        case (rt_uint32_t)WHD_SECURITY_WPA_AES_ENT:
        case (rt_uint32_t)WHD_SECURITY_WPA_MIXED_ENT:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA_ENTERPRISE;

        case (rt_uint32_t)WHD_SECURITY_WPA2_TKIP_ENT:
        case (rt_uint32_t)WHD_SECURITY_WPA2_AES_ENT:
        case (rt_uint32_t)WHD_SECURITY_WPA2_MIXED_ENT:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_ENTERPRISE;

        case (rt_uint32_t)WHD_SECURITY_WPA3_SAE:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_AES_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA3_WPA2_PSK:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_WPA3_PSK;

        case (rt_uint32_t)WHD_SECURITY_WPA3_ENT:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_ENTERPRISE;

        case (rt_uint32_t)WHD_SECURITY_WPA3_ENT_AES_CCMP:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_WPA3_ENTERPRISE;

        case (rt_uint32_t)WHD_SECURITY_WPA3_192BIT_ENT:
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_ENT_192;

        case (rt_uint32_t)WHD_SECURITY_WPA3_OWE:
            return (rt_wlan_security_t)AERO_RTT_SEC_OWE;

        case (rt_uint32_t)WHD_SECURITY_IBSS_OPEN:
            return SECURITY_OPEN;

        case (rt_uint32_t)WHD_SECURITY_WPS_SECURE:
            return SECURITY_WPS_SECURE;

        default:
            break;
    }

    /* Fallback: the driver reports the security of a BSS as a union of
     * flags, not as one of the whd_security_t enum values. The RSN parser
     * in whd_wifi_api.c ORs together a bit for every AKM suite and every
     * cipher suite the AP advertises, and it may additionally set modifier
     * flags (SHA256_1X, FBT, SUITE_B).
     *
     * A WPA2/WPA3 mixed AP that also supports 802.11r and SHA-256 reports,
     * for example, 0x45400004 (WPA2|WPA3|SHA256_1X|AES) plus FBT, which
     * does not equal any single enum entry. Matching on the whole value
     * would classify such an AP as SECURITY_UNKNOWN.
     *
     * The value is therefore classified by its family bits: the family
     * bits (WEP / WPA / WPA2 / WPA3 / enterprise / OWE) plus the cipher
     * bits decide the result, the modifier flags are ignored. */
    {
        rt_bool_t has_wpa   = (sec & AERO_WHD_SEC_WPA) != 0U;
        rt_bool_t has_wpa2  = (sec & AERO_WHD_SEC_WPA2) != 0U;
        rt_bool_t has_wpa3  = (sec & AERO_WHD_SEC_WPA3) != 0U;
        rt_bool_t has_ent   = (sec & AERO_WHD_SEC_ENTERPRISE) != 0U;
        rt_bool_t has_tkip  = (sec & AERO_WHD_SEC_TKIP) != 0U;
        rt_bool_t has_aes   = (sec & AERO_WHD_SEC_AES) != 0U;

        /* WHD_SECURITY_UNKNOWN is -1, so every bit is set. It must be
         * rejected before the family bits are examined, otherwise it would
         * be mistaken for a WPA2/WPA3 enterprise network. */
        if (sec == (rt_uint32_t)WHD_SECURITY_UNKNOWN)
        {
            return SECURITY_UNKNOWN;
        }

        /* OWE is an enhanced-open network, reported with its own bit. */
        if ((sec & AERO_WHD_FLAG_OWE) != 0U)
        {
            return (rt_wlan_security_t)AERO_RTT_SEC_OWE;
        }

        /* WPA3 Enterprise 192-bit (Suite B) has its own framework value. */
        if ((sec & AERO_WHD_FLAG_SUITE_B_SHA384) != 0U)
        {
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_ENT_192;
        }

        /* Enterprise: 802.1X based. */
        if (has_ent)
        {
            if (has_wpa3 && has_wpa2)
            {
                return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_WPA3_ENTERPRISE;
            }
            if (has_wpa3)
            {
                return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_ENTERPRISE;
            }
            if (has_wpa2)
            {
                return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_ENTERPRISE;
            }
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA_ENTERPRISE;
        }

        /* Personal (PSK/SAE) networks. */
        if (has_wpa3 && has_wpa2)
        {
            /* WPA2 + WPA3 transition mode: the AP accepts SAE. */
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA2_WPA3_PSK;
        }
        if (has_wpa3)
        {
            return (rt_wlan_security_t)AERO_RTT_SEC_WPA3_AES_PSK;
        }
        if (has_wpa2)
        {
            /* WPA2 family: distinguish pure TKIP / pure AES / mixed.
             * The driver sets TKIP when the AP also advertises TKIP and
             * AES when it advertises CCMP, so "mixed" means both. */
            if (has_tkip && has_aes)
            {
                return SECURITY_WPA2_MIXED_PSK;
            }
            if (has_tkip)
            {
                return SECURITY_WPA2_TKIP_PSK;
            }
            return SECURITY_WPA2_AES_PSK;
        }
        if (has_wpa)
        {
            /* WPA (legacy) family. */
            if (has_tkip && has_aes)
            {
                return SECURITY_WPA2_MIXED_PSK;
            }
            if (has_tkip)
            {
                return SECURITY_WPA_TKIP_PSK;
            }
            return SECURITY_WPA_AES_PSK;
        }

        /* WEP. */
        if ((sec & AERO_WHD_SEC_WEP) != 0U)
        {
            return SECURITY_WEP_PSK;
        }
    }

    return SECURITY_UNKNOWN;
}

/**
 * @brief Normalize a raw WHD security bitmask into a canonical WHD enum value
 *
 * The scan code in whd_wifi_api.c builds the security of a BSS as a union of
 * flags (one bit per AKM suite, one per cipher suite, plus modifier flags
 * such as SHA256_1X / FBT / SUITE_B). Such a value must not be handed to
 * whd_wifi_prepare_join() directly: the driver switches on the whole value
 * and would fall into its "Unsupported security type" assert.
 *
 * This function maps the raw mask onto the canonical whd_security_t value
 * that the driver understands.
 */
static whd_security_t whd_normalize_security(whd_security_t raw)
{
    rt_uint32_t sec = (rt_uint32_t)raw;
    rt_bool_t has_wpa    = (sec & AERO_WHD_SEC_WPA) != 0U;
    rt_bool_t has_wpa2   = (sec & AERO_WHD_SEC_WPA2) != 0U;
    rt_bool_t has_wpa3   = (sec & AERO_WHD_SEC_WPA3) != 0U;
    rt_bool_t has_ent    = (sec & AERO_WHD_SEC_ENTERPRISE) != 0U;
    rt_bool_t has_tkip   = (sec & AERO_WHD_SEC_TKIP) != 0U;
    rt_bool_t has_aes    = (sec & AERO_WHD_SEC_AES) != 0U;
    rt_bool_t has_sha256 = (sec & AERO_WHD_FLAG_SHA256_1X) != 0U;

    /* WHD_SECURITY_UNKNOWN is -1, so every bit is set. It must be
     * rejected before the family bits are examined, otherwise it would
     * be mistaken for a WPA2/WPA3 enterprise network. */
    if (sec == (rt_uint32_t)WHD_SECURITY_UNKNOWN)
    {
        return WHD_SECURITY_UNKNOWN;
    }

    /* OWE (enhanced open). */
    if ((sec & AERO_WHD_FLAG_OWE) != 0U)
    {
        return WHD_SECURITY_WPA3_OWE;
    }

    /* WPA3 Enterprise 192-bit (Suite B). */
    if ((sec & AERO_WHD_FLAG_SUITE_B_SHA384) != 0U)
    {
        return WHD_SECURITY_WPA3_192BIT_ENT;
    }

    /* Enterprise (802.1X). */
    if (has_ent)
    {
        if (has_wpa3 && has_wpa2)
        {
            return WHD_SECURITY_WPA3_ENT_AES_CCMP;
        }
        if (has_wpa3)
        {
            return WHD_SECURITY_WPA3_ENT;
        }
        if (has_wpa2)
        {
            return WHD_SECURITY_WPA2_AES_ENT;
        }
        return WHD_SECURITY_WPA_AES_ENT;
    }

    /* Personal (PSK / SAE).
     *
     * The FBT (802.11r) bit is deliberately dropped: the porting layer
     * detects the SAE / WPA3 transition case by comparing against the
     * canonical WHD_SECURITY_WPA3_SAE and WHD_SECURITY_WPA3_WPA2_PSK values,
     * and the driver only takes the "set passphrase for SAE" path for
     * WHD_SECURITY_WPA3_WPA2_PSK. Returning the FBT variants would make
     * both of those exact comparisons fail and the WPA3 join would then be
     * attempted as a plain WPA2 join. Fast BSS transition is optional for
     * the initial association, so the non-FBT value is used. */
    if (has_wpa3 && has_wpa2)
    {
        /* WPA2 + WPA3 transition mode: the AP accepts SAE. */
        return WHD_SECURITY_WPA3_WPA2_PSK;
    }
    if (has_wpa3)
    {
        return WHD_SECURITY_WPA3_SAE;
    }
    if (has_wpa2)
    {
        if (has_tkip && has_aes)
        {
            return WHD_SECURITY_WPA2_MIXED_PSK;
        }
        if (has_tkip)
        {
            return WHD_SECURITY_WPA2_TKIP_PSK;
        }
        if (has_sha256)
        {
            return WHD_SECURITY_WPA2_AES_PSK_SHA256;
        }
        return WHD_SECURITY_WPA2_AES_PSK;
    }
    if (has_wpa)
    {
        if (has_tkip && has_aes)
        {
            return WHD_SECURITY_WPA_MIXED_PSK;
        }
        if (has_tkip)
        {
            return WHD_SECURITY_WPA_TKIP_PSK;
        }
        return WHD_SECURITY_WPA_AES_PSK;
    }

    /* WEP / Open. */
    if ((sec & AERO_WHD_SEC_WEP) != 0U)
    {
        return WHD_SECURITY_WEP_PSK;
    }

    return WHD_SECURITY_UNKNOWN;
}
static whd_security_t rtt_to_whd_security(rt_wlan_security_t rtt_sec)
{
    rt_uint32_t sec = (rt_uint32_t)rtt_sec;

    switch (sec)
    {
        case (rt_uint32_t)SECURITY_OPEN:
            return WHD_SECURITY_OPEN;
        case (rt_uint32_t)SECURITY_WEP_PSK:
        case (rt_uint32_t)SECURITY_WEP_SHARED:
            return WHD_SECURITY_WEP_PSK;
        case (rt_uint32_t)SECURITY_WPA_TKIP_PSK:
            return WHD_SECURITY_WPA_TKIP_PSK;
        case (rt_uint32_t)SECURITY_WPA_AES_PSK:
            return WHD_SECURITY_WPA_AES_PSK;
        case (rt_uint32_t)SECURITY_WPA2_AES_PSK:
            return WHD_SECURITY_WPA2_AES_PSK;
        case (rt_uint32_t)SECURITY_WPA2_TKIP_PSK:
            return WHD_SECURITY_WPA2_TKIP_PSK;
        case (rt_uint32_t)SECURITY_WPA2_MIXED_PSK:
            return WHD_SECURITY_WPA2_MIXED_PSK;
        case (rt_uint32_t)SECURITY_WPS_OPEN:
            return WHD_SECURITY_OPEN;
        case (rt_uint32_t)SECURITY_WPS_SECURE:
            return WHD_SECURITY_WPS_SECURE;
        default:
            break;
    }

    /* The framework security types for WPA3, enterprise and OWE are matched
     * by value, so that this file compiles even when the framework does not
     * provide these types. */
    switch (sec)
    {
        case AERO_RTT_SEC_WPA_ENTERPRISE:
            return WHD_SECURITY_WPA_AES_ENT;
        case AERO_RTT_SEC_WPA2_ENTERPRISE:
            return WHD_SECURITY_WPA2_AES_ENT;
        case AERO_RTT_SEC_WPA3_AES_PSK:
            return WHD_SECURITY_WPA3_SAE;
        case AERO_RTT_SEC_WPA2_WPA3_PSK:
            return WHD_SECURITY_WPA3_WPA2_PSK;
        case AERO_RTT_SEC_WPA3_ENTERPRISE:
            return WHD_SECURITY_WPA3_ENT;
        case AERO_RTT_SEC_WPA2_WPA3_ENTERPRISE:
            return WHD_SECURITY_WPA3_ENT_AES_CCMP;
        case AERO_RTT_SEC_WPA3_ENT_192:
            return WHD_SECURITY_WPA3_192BIT_ENT;
        case AERO_RTT_SEC_OWE:
            return WHD_SECURITY_WPA3_OWE;
        default:
            return WHD_SECURITY_UNKNOWN;
    }
}

/**
 * @brief Convert WHD band to RT-Thread band
 */
static rt_802_11_band_t whd_to_rtt_band(whd_802_11_band_t whd_band)
{
    switch (whd_band)
    {
        case WHD_802_11_BAND_2_4GHZ:
            return RT_802_11_BAND_2_4GHZ;
        case WHD_802_11_BAND_5GHZ:
            return RT_802_11_BAND_5GHZ;
        default:
            return RT_802_11_BAND_UNKNOWN;
    }
}

/**
 * @brief Check if security type requires 4-way handshake
 */
static rt_bool_t security_requires_handshake(whd_security_t security)
{
    rt_uint32_t sec = (rt_uint32_t)security;

    switch (sec)
    {
        case (rt_uint32_t)WHD_SECURITY_OPEN:
        case (rt_uint32_t)WHD_SECURITY_IBSS_OPEN:
        case (rt_uint32_t)WHD_SECURITY_WPS_SECURE:
        case (rt_uint32_t)WHD_SECURITY_WEP_PSK:
        case (rt_uint32_t)WHD_SECURITY_WEP_SHARED:
            return RT_FALSE;

        default:
            break;
    }

    /* Everything else (WPA/WPA2/WPA3 personal and enterprise) needs the
     * 4-way handshake. The same is assumed for an unknown security type. */
    return RT_TRUE;
}

/*******************************************************************************
 * Event Handlers
 ******************************************************************************/

/**
 * @brief STA link event handler
 */
static void* sta_event_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                                const uint8_t *event_data, void *handler_user_data)
{
    drv_wifi_t *wifi = (drv_wifi_t *)handler_user_data;

    if (wifi == NULL || wifi->wlan == NULL)
        return handler_user_data;

    LOG_D("STA event: type=%lu, status=%lu, reason=%lu, flags=%lu",
          event_header->event_type, event_header->status,
          event_header->reason, event_header->flags);

    /* ---- CONNECTING-state guard ----
     *
     * This handler is registered at init and stays active throughout the
     * join sequence, so transient link/deauth events that occur while the
     * firmware is still authenticating/associating must be filtered out here
     * to avoid reporting spurious connect/disconnect notifications to the
     * framework. All events are still logged.
     *
     * WLC_E_PSK_SUP is always processed because that is the final event
     * that signals a successful WPA handshake.
     */
    if (wifi->state == WIFI_STATE_CONNECTING)
    {
        switch (event_header->event_type)
        {
            case WLC_E_LINK:
                if (event_header->flags & WLC_EVENT_MSG_LINK)
                {
                    LOG_I("STA: Link up event (joining)");
                    if (!security_requires_handshake(sta_security_type))
                    {
                        /* OPEN/WEP: link-up IS the final event */
                        link_up_handler(wifi);
                    }
                    else
                    {
                        /* WPA: track flag, wait for PSK_SUP */
                        link_up_event_received = RT_TRUE;
                    }
                }
                else
                {
                    /* Transient link-down during join - firmware internal */
                    LOG_I("STA: Link down during join, reason=%lu", event_header->reason);
                    link_up_event_received = RT_FALSE;
                }
                break;

            case WLC_E_PSK_SUP:
                goto handle_psk_sup;  /* always process handshake result */

            case WLC_E_DEAUTH_IND:
                LOG_I("STA: Deauth during join, reason=%lu", event_header->reason);
                break;

            case WLC_E_DISASSOC_IND:
                LOG_I("STA: Disassoc during join, reason=%lu", event_header->reason);
                break;

            default:
                LOG_D("STA: Event %lu during join (ignored)",
                      event_header->event_type);
                break;
        }
        return handler_user_data;
    }

    switch (event_header->event_type)
    {
        case WLC_E_LINK:
            if (event_header->flags & WLC_EVENT_MSG_LINK)
            {
                /* Link up event received */
                LOG_I("STA: Link up event");

                if (!security_requires_handshake(sta_security_type))
                {
                    /* No handshake required, advertise link up immediately */
                    link_up_handler(wifi);
                }
                else
                {
                    /* Wait for WLC_E_PSK_SUP to complete 4-way handshake */
                    link_up_event_received = RT_TRUE;
                    LOG_D("Waiting for 4-way handshake...");
                }
            }
            else
            {
                /* Link down */
                if (event_header->reason == WLC_E_LINK_BCN_LOSS)
                {
                    LOG_W("STA: Beacon lost");
                }
                else
                {
                    LOG_I("STA: Link down, reason=%lu", event_header->reason);
                }
                link_down_handler(wifi, event_header->reason);
            }
            break;

        case WLC_E_DEAUTH_IND:
            LOG_W("STA: Deauth indication, reason=%lu", event_header->reason);
            link_down_handler(wifi, event_header->reason);
            break;

        case WLC_E_DISASSOC_IND:
            LOG_W("STA: Disassoc indication, reason=%lu", event_header->reason);
            link_down_handler(wifi, event_header->reason);
            break;

        case WLC_E_PSK_SUP:
        handle_psk_sup:
            /* 4-way handshake status */
            LOG_D("STA: PSK_SUP event, status=%lu, reason=%lu",
                  event_header->status, event_header->reason);

            if (event_header->status == WLC_SUP_KEYED &&
                event_header->reason == WLC_E_SUP_OTHER &&
                link_up_event_received == RT_TRUE)
            {
                /* Successful handshake */
                LOG_I("STA: 4-way handshake complete");
                link_up_event_received = RT_FALSE;
                link_up_handler(wifi);
            }
            else if (event_header->reason == WLC_E_SUP_MSG3_TOO_MANY_IE)
            {
                /* Too many IE error - log warning */
                LOG_W("STA: Too many RSN IE in handshake");
            }
            break;

        case WLC_E_CSA_COMPLETE_IND:
            /* Channel Switch Announcement complete
             * Firmware handles CSA automatically - no driver action needed.
             * The connection remains valid after channel switch. */
            LOG_I("STA: CSA complete (channel switched by firmware)");
            break;

        default:
            break;
    }

    return handler_user_data;
}

/**
 * @brief Check if AP security is OPEN/WEP (no 4-way handshake needed)
 */
static rt_bool_t ap_security_is_open_or_wep(whd_security_t security)
{
    switch (security)
    {
        case WHD_SECURITY_OPEN:
        case WHD_SECURITY_WEP_PSK:
        case WHD_SECURITY_WEP_SHARED:
            return RT_TRUE;
        default:
            return RT_FALSE;
    }
}

static const char* ap_event_type_to_str(uint32_t event_type)
{
    switch (event_type)
    {
        case WLC_E_DISASSOC_IND:
            return "DISASSOC_IND";
        case WLC_E_DEAUTH_IND:
            return "DEAUTH_IND";
        case WLC_E_ASSOC_IND:
            return "ASSOC_IND";
        case WLC_E_REASSOC_IND:
            return "REASSOC_IND";
        case WLC_E_AUTHORIZED:
            return "AUTHORIZED";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief AP link event handler
 *
 * For OPEN/WEP security: Report client join on ASSOC_IND/REASSOC_IND
 * For WPA/WPA2/WPA3: Wait for AUTHORIZED event before reporting client join
 */
static void* ap_event_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                               const uint8_t *event_data, void *handler_user_data)
{
    drv_wifi_t *wifi = (drv_wifi_t *)handler_user_data;
    struct rt_wlan_buff buff;
    struct rt_wlan_info info;
    rt_bool_t ap_up_snapshot = RT_FALSE;
    rt_bool_t sta_up_snapshot = RT_FALSE;
    rt_bool_t roaming_enabled_snapshot = RT_FALSE;
    rt_bool_t roam_forced_off_snapshot = RT_FALSE;
    whd_security_t ap_sec_snapshot = WHD_SECURITY_UNKNOWN;

    if (wifi == NULL || wifi->wlan == NULL)
        return handler_user_data;

    if (wifi_lock() == RT_EOK)
    {
        ap_up_snapshot = ap_link_up;
        sta_up_snapshot = sta_link_up;
        roaming_enabled_snapshot = roaming_enabled;
        roam_forced_off_snapshot = sta_roam_forced_off_by_ap;
        ap_sec_snapshot = ap_security_type;
        wifi_unlock();
    }
    else
    {
        ap_up_snapshot = ap_link_up;
        sta_up_snapshot = sta_link_up;
        roaming_enabled_snapshot = roaming_enabled;
        roam_forced_off_snapshot = sta_roam_forced_off_by_ap;
        ap_sec_snapshot = ap_security_type;
    }

    LOG_D("AP event: %s type=%lu status=%lu reason=%lu ap_up=%d sta_up=%d ap_sec=%d roam_en=%d roam_forced_off=%d",
          ap_event_type_to_str(event_header->event_type),
          event_header->event_type, event_header->status, event_header->reason,
          ap_up_snapshot, sta_up_snapshot, ap_sec_snapshot,
          roaming_enabled_snapshot, roam_forced_off_snapshot);

    /* Prepare client info */
    memset(&info, 0, sizeof(info));
    memcpy(info.bssid, event_header->addr.octet, RT_WLAN_BSSID_MAX_LENGTH);
    buff.data = &info;
    buff.len = sizeof(info);

    switch (event_header->event_type)
    {
        case WLC_E_DISASSOC_IND:
        case WLC_E_DEAUTH_IND:
            /* Client left - always report */
            LOG_I("AP: Client left (%s): %02X:%02X:%02X:%02X:%02X:%02X (status=%lu reason=%lu ap_up=%d sta_up=%d ap_sec=%d roam_en=%d roam_forced_off=%d)",
                  ap_event_type_to_str(event_header->event_type),
                  event_header->addr.octet[0], event_header->addr.octet[1],
                  event_header->addr.octet[2], event_header->addr.octet[3],
                  event_header->addr.octet[4], event_header->addr.octet[5],
                  event_header->status, event_header->reason,
                  ap_up_snapshot, sta_up_snapshot, ap_sec_snapshot,
                  roaming_enabled_snapshot, roam_forced_off_snapshot);

            rt_wlan_dev_indicate_event_handle(wifi->wlan, RT_WLAN_DEV_EVT_AP_DISASSOCIATED, &buff);
            break;

        case WLC_E_ASSOC_IND:
        case WLC_E_REASSOC_IND:
            /* Client associated - only report for OPEN/WEP security.
             * For WPA/WPA2, wait for the AUTHORIZED event instead. */
            if (ap_security_is_open_or_wep(ap_security_type))
            {
                LOG_I("AP: Client joined (OPEN/WEP): %02X:%02X:%02X:%02X:%02X:%02X",
                      event_header->addr.octet[0], event_header->addr.octet[1],
                      event_header->addr.octet[2], event_header->addr.octet[3],
                      event_header->addr.octet[4], event_header->addr.octet[5]);

                rt_wlan_dev_indicate_event_handle(wifi->wlan, RT_WLAN_DEV_EVT_AP_ASSOCIATED, &buff);
            }
            else
            {
                LOG_I("AP: Client associated (%s, WPA*): %02X:%02X:%02X:%02X:%02X:%02X (status=%lu reason=%lu)",
                      ap_event_type_to_str(event_header->event_type),
                      event_header->addr.octet[0], event_header->addr.octet[1],
                      event_header->addr.octet[2], event_header->addr.octet[3],
                      event_header->addr.octet[4], event_header->addr.octet[5],
                      event_header->status, event_header->reason);
            }
            break;

        case WLC_E_AUTHORIZED:
            /* Client completed 4-way handshake - report for WPA/WPA2/WPA3 */
            if (!ap_security_is_open_or_wep(ap_security_type))
            {
                LOG_I("AP: Client authorized (WPA): %02X:%02X:%02X:%02X:%02X:%02X (status=%lu reason=%lu ap_up=%d sta_up=%d)",
                      event_header->addr.octet[0], event_header->addr.octet[1],
                      event_header->addr.octet[2], event_header->addr.octet[3],
                      event_header->addr.octet[4], event_header->addr.octet[5],
                      event_header->status, event_header->reason,
                      ap_up_snapshot, sta_up_snapshot);

                rt_wlan_dev_indicate_event_handle(wifi->wlan, RT_WLAN_DEV_EVT_AP_ASSOCIATED, &buff);
            }
            break;

        default:
            break;
    }

    return handler_user_data;
}

/**
 * @brief Link up event handler
 */
static void link_up_handler(drv_wifi_t *wifi)
{
    if (wifi == &wifi_sta)
    {
        if (wifi_lock() == RT_EOK)
        {
            sta_link_up = RT_TRUE;
            wifi->state = WIFI_STATE_CONNECTED;
            wifi_unlock();
        }
        else
        {
            sta_link_up = RT_TRUE;
            wifi->state = WIFI_STATE_CONNECTED;
        }

        /* Notify RT-Thread WLAN framework */
        rt_wlan_dev_indicate_event_handle(wifi->wlan, RT_WLAN_DEV_EVT_CONNECT, RT_NULL);
    }
}

/**
 * @brief Link down event handler
 */
static void link_down_handler(drv_wifi_t *wifi, uint32_t reason)
{
    rt_bool_t was_connected = RT_FALSE;

    if (wifi == &wifi_sta)
    {
        if (wifi_lock() == RT_EOK)
        {
            if (!sta_link_up)
            {
                wifi_unlock();
                return;  /* Already disconnected */
            }
            was_connected = RT_TRUE;
            sta_link_up = RT_FALSE;
            wifi->state = WIFI_STATE_IDLE;
            wifi_unlock();
        }
        else
        {
            if (!sta_link_up)
                return;  /* Already disconnected */
            was_connected = RT_TRUE;
            sta_link_up = RT_FALSE;
            wifi->state = WIFI_STATE_IDLE;
        }

        if (was_connected)
        {
            /* Notify RT-Thread WLAN framework */
            rt_wlan_dev_indicate_event_handle(wifi->wlan, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL);
        }

        /* Clear connected AP info if intentional disconnect */
        /* For unexpected disconnect, keep info for auto-reconnect */
    }
}

/*******************************************************************************
 * Scan Functions
 ******************************************************************************/

/**
 * @brief Internal scan callback
 */
static void scan_callback(whd_scan_result_t **result_ptr, void *user_data, whd_scan_status_t status)
{
    scan_context_t *ctx = (scan_context_t *)user_data;
    rt_sem_t sem;

    if (ctx == NULL || ctx->wlan == NULL)
        return;

    /* Check for completion */
    if ((result_ptr == NULL) || (*result_ptr == NULL))
    {
        if (status == WHD_SCAN_COMPLETED_SUCCESSFULLY || status == WHD_SCAN_ABORTED)
        {
            if (ctx->scan_type == WHD_SCAN_TYPE_ACTIVE && !ctx->stop_requested && ctx->run_passive)
            {
                /* Active scan complete, now do passive scan */
                sem = RT_NULL;
                if (wifi_lock() == RT_EOK)
                {
                    if (ctx->scan_type == WHD_SCAN_TYPE_ACTIVE && !ctx->stop_requested && ctx->run_passive)
                    {
                        ctx->scan_type = WHD_SCAN_TYPE_PASSIVE;
                        memset(&ctx->scan_result, 0, sizeof(ctx->scan_result));
                        sem = ctx->scan_complete_sem;
                    }
                    wifi_unlock();
                }
                if (sem != RT_NULL)
                    rt_sem_release(sem);
            }
            else
            {
                /* In managed mode the driver supervises the scan itself:
                 * the fast scan may have to be repeated with the firmware
                 * default dwell times.
                 *
                 * The callback does NOT report SCAN_DONE. It only records
                 * the state and wakes the driver thread up, which reports
                 * SCAN_DONE exactly once at the end of the whole scan
                 * sequence. */
                if (ctx->managed)
                {
                    if (wifi_lock() == RT_EOK)
                    {
                        switch (ctx->stage)
                        {
                        case WHD_SCAN_STAGE_FAST:
                            /* The fast scan finished. Wake the driver up,
                             * it decides whether a fallback scan is needed. */
                            ctx->stage = WHD_SCAN_STAGE_FALLBACK;
                            break;

                        case WHD_SCAN_STAGE_FALLBACK_RUN:
                            /* The fallback scan finished. This is the last
                             * scan of the sequence. */
                            ctx->stage = WHD_SCAN_STAGE_DONE;
                            ctx->is_scanning = RT_FALSE;
                            break;

                        case WHD_SCAN_STAGE_FALLBACK:
                        case WHD_SCAN_STAGE_DONE:
                        case WHD_SCAN_STAGE_IDLE:
                        default:
                            /* A duplicated (or late) completion callback.
                             * It must not change the state of the sequence,
                             * it may still be running. */
                            break;
                        }
                        sem = ctx->scan_complete_sem;
                        wifi_unlock();
                    }
                    else
                    {
                        sem = ctx->scan_complete_sem;
                    }

                    if (sem != RT_NULL)
                        rt_sem_release(sem);
                    return;
                }

                /* All scanning complete */
                LOG_D("Scan complete");
                {
                    rt_bool_t report_done = RT_FALSE;

                    if (wifi_lock() == RT_EOK)
                    {
                        ctx->is_scanning = RT_FALSE;
                        if (!ctx->done_reported)
                        {
                            ctx->done_reported = RT_TRUE;
                            report_done = RT_TRUE;
                        }
                        wifi_unlock();
                    }
                    else
                    {
                        ctx->is_scanning = RT_FALSE;
                    }

                    if (report_done)
                    {
                        rt_wlan_dev_indicate_event_handle(ctx->wlan, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL);
                    }
                }
            }
        }
        return;
    }

    /* Process scan result */
    whd_scan_result_t *bss_info = *result_ptr;
    struct rt_wlan_info wlan_info;
    struct rt_wlan_buff buff;
    rt_uint8_t ssid_len;

    memset(&wlan_info, 0, sizeof(wlan_info));

    /* Remember whether the SSID this scan is looking for was seen. The
     * fast (short dwell) join-path scan uses this to decide whether it has
     * to be repeated with the firmware default dwell times, and the early
     * stop uses it to decide when the scan may be aborted. */
    if (ctx->has_target_ssid && !ctx->target_found)
    {
        if (bss_info->SSID.length == ctx->target_ssid.length &&
            memcmp(bss_info->SSID.value, ctx->target_ssid.value, bss_info->SSID.length) == 0)
        {
            if (wifi_lock() == RT_EOK)
            {
                ctx->target_found = RT_TRUE;
                ctx->target_found_tick = rt_tick_get();
                wifi_unlock();
            }
            else
            {
                ctx->target_found = RT_TRUE;
                ctx->target_found_tick = rt_tick_get();
            }

            /* Remember the channel of the target AP. The next join-path
             * scan for the same SSID can then be restricted to this
             * channel instead of sweeping the whole band. */
            scan_channel_cache_store(&ctx->target_ssid, bss_info->channel);
        }
    }

    /* Copy BSSID */
    memcpy(wlan_info.bssid, bss_info->BSSID.octet, sizeof(wlan_info.bssid));

    /* Copy SSID */
    ssid_len = (rt_uint8_t)bss_info->SSID.length;
    if (ssid_len > RT_WLAN_SSID_MAX_LENGTH)
        ssid_len = RT_WLAN_SSID_MAX_LENGTH;
    if (ssid_len > 0)
        memcpy(wlan_info.ssid.val, bss_info->SSID.value, ssid_len);
    wlan_info.ssid.val[ssid_len] = 0;
    wlan_info.ssid.len = ssid_len;
    wlan_info.hidden = (bss_info->SSID.length == 0) ? 1 : 0;

    /* Other info */
    wlan_info.channel = (rt_int16_t)bss_info->channel;
    wlan_info.rssi = bss_info->signal_strength;
    wlan_info.datarate = bss_info->max_data_rate * 1000;
    wlan_info.band = whd_to_rtt_band(bss_info->band);
    wlan_info.security = whd_to_rtt_security(bss_info->security);


    LOG_D("scan: ssid=%s bssid=%02X:%02X:%02X:%02X:%02X:%02X whd_sec=0x%08lX rtt_sec=0x%08lX",
          (char *)wlan_info.ssid.val,
          wlan_info.bssid[0], wlan_info.bssid[1], wlan_info.bssid[2],
          wlan_info.bssid[3], wlan_info.bssid[4], wlan_info.bssid[5],
          (unsigned long)(rt_uint32_t)bss_info->security,
          (unsigned long)(rt_uint32_t)wlan_info.security);

    if (wlan_info.security == SECURITY_UNKNOWN)
    {
        LOG_W("scan: UNMAPPED security 0x%08lX for ssid=%s",
              (unsigned long)(rt_uint32_t)bss_info->security,
              (char *)wlan_info.ssid.val);
    }

    /* Report to framework */
    buff.data = &wlan_info;
    buff.len = sizeof(wlan_info);
    rt_wlan_dev_indicate_event_handle(ctx->wlan, RT_WLAN_DEV_EVT_SCAN_REPORT, &buff);
}

/*******************************************************************************
 * Network Data Functions
 ******************************************************************************/


#ifndef WHD_USE_ORIGINAL_DRIVER

/* Forward declaration for lwip_prot_des structure from wlan_lwip.c */
struct lwip_prot_des {
    struct rt_wlan_prot prot;
    struct eth_device eth;
    rt_int8_t connected_flag;
    struct rt_timer timer;
    struct rt_work work;
};

void cy_network_process_ethernet_data(whd_interface_t iface, whd_buffer_t buf)
{
    if (iface == NULL || buf == NULL)
        return;

    struct pbuf *p = (struct pbuf *)buf;
    uint8_t *data = (uint8_t *)p->payload;
    uint16_t data_len = p->tot_len;

    struct rt_wlan_device *wlan = NULL;

    if (iface == wifi_sta.whd_itf)
        wlan = wifi_sta.wlan;
    else if (iface == wifi_ap.whd_itf)
        wlan = wifi_ap.wlan;

    if (wlan == NULL)
    {
        whd_buffer_release(iface->whd_driver, buf, WHD_NETWORK_RX);
        return;
    }

    /* Check for EAPOL packets - must go through WLAN framework for WPA */
    uint16_t ethertype = (uint16_t)(data[12] << 8 | data[13]);
    #define EAPOL_PACKET_TYPE   (0x888E)

    if (ethertype == EAPOL_PACKET_TYPE)
    {
        rt_wlan_dev_report_data(wlan, data, data_len);
        whd_buffer_release(iface->whd_driver, buf, WHD_NETWORK_RX);
        return;
    }


    if (wlan->prot != RT_NULL)
    {
        struct lwip_prot_des *lwip_prot = (struct lwip_prot_des *)wlan->prot;
        struct netif *netif = lwip_prot->eth.netif;

        if (netif != RT_NULL && netif->input != RT_NULL)
        {
            /* ref+1 so both lwIP and whd_buffer_release can pbuf_free independently */
            pbuf_ref(p);

            if (netif->input(p, netif) != ERR_OK)
            {
                /* tcpip mailbox full - drop the extra reference just added */
                pbuf_free(p);
            }

            /* Release WHD's reference (ref-1) */
            whd_buffer_release(iface->whd_driver, buf, WHD_NETWORK_RX);
            return;
        }
    }

    /* Fallback: use WLAN framework path (copies data) */
    rt_wlan_dev_report_data(wlan, data, data_len);
    whd_buffer_release(iface->whd_driver, buf, WHD_NETWORK_RX);
}
#else
/* When using original driver, declare external reference */
extern void cy_network_process_ethernet_data(whd_interface_t iface, whd_buffer_t buf);
#endif

static whd_netif_funcs_t netif_if_ops = {
    .whd_network_process_ethernet_data = cy_network_process_ethernet_data,
};

/*******************************************************************************
 * RT-Thread WLAN Device Operations
 ******************************************************************************/

static rt_err_t drv_wlan_init(struct rt_wlan_device *wlan)
{
    return RT_EOK;
}

static rt_err_t drv_wlan_mode(struct rt_wlan_device *wlan, rt_wlan_mode_t mode)
{
    return RT_EOK;
}

/**
 * @brief Convert milliseconds to ticks, never returning 0
 *
 * Used for the deadline arithmetic of the managed scan sequence.
 */
static rt_tick_t scan_ms_to_ticks(rt_int32_t ms)
{
    rt_tick_t ticks;

    if (ms <= 0)
        return 1;

    ticks = rt_tick_from_millisecond(ms);
    if (ticks == 0)
        ticks = 1;

    return ticks;
}

/**
 * @brief Number of ticks elapsed since the given start tick
 *
 * The difference is computed on the 32 bit tick counter, so it stays
 * correct when the counter wraps around. The result is only valid while
 * the elapsed time is below 2^31 ticks.
 */
static rt_int32_t scan_ticks_elapsed(rt_tick_t start)
{
    return (rt_int32_t)(rt_tick_get() - start);
}

/**
 * @brief Drain all pending tokens of a semaphore
 *
 * A scan callback may release the completion semaphore more than once
 * (for example when a scan is aborted, which produces an extra callback).
 * Before the semaphore is reused for the next scan of a sequence, all
 * pending tokens have to be removed, otherwise the waiter would continue
 * immediately and the scan would not be awaited at all.
 */
static void scan_sem_drain(rt_sem_t sem)
{
    if (sem == RT_NULL)
        return;

    while (rt_sem_take(sem, 0) == RT_EOK)
    {
        /* keep draining */
    }
}

/**
 * @brief Remember the channel an AP was seen on
 *
 * The cache is used to speed up the next join-path scan for the same SSID.
 * Only the channel is cached, the BSSID and the security type are always
 * taken from the scan result.
 */
static void scan_channel_cache_store(const whd_ssid_t *ssid, uint8_t channel)
{
    rt_int32_t i;
    rt_int32_t slot = 0;
    rt_bool_t  slot_found = RT_FALSE;
    rt_bool_t  locked = RT_FALSE;

    if (ssid == RT_NULL || channel == 0)
        return;

    /* The cache is written by the scan callback (WHD thread) and read by
     * the thread that starts the scan, so it is protected by the WiFi
     * mutex. The lock is not taken when it is not available (during the
     * start up), the cache is only a hint in that case. */
    if (wifi_lock() == RT_EOK)
    {
        locked = RT_TRUE;
    }

    /* Update an existing entry. */
    for (i = 0; i < WHD_SCAN_CHANNEL_CACHE_ENTRIES; i++)
    {
        scan_channel_cache_t *entry = &scan_channel_cache_tab[i];

        if (entry->valid &&
            entry->ssid.length == ssid->length &&
            memcmp(entry->ssid.value, ssid->value, ssid->length) == 0)
        {
            entry->channel = channel;
            entry->age_ticks = rt_tick_get();
            if (locked)
            {
                wifi_unlock();
            }
            return;
        }
    }

    /* Use a free slot, or replace the oldest entry. */
    for (i = 0; i < WHD_SCAN_CHANNEL_CACHE_ENTRIES; i++)
    {
        scan_channel_cache_t *entry = &scan_channel_cache_tab[i];

        if (!entry->valid)
        {
            slot = i;
            slot_found = RT_TRUE;
            break;
        }

        if (!slot_found ||
            scan_ticks_elapsed(entry->age_ticks) > scan_ticks_elapsed(scan_channel_cache_tab[slot].age_ticks))
        {
            slot = i;
            slot_found = RT_TRUE;
        }
    }

    {
        scan_channel_cache_t *entry = &scan_channel_cache_tab[slot];

        entry->valid = RT_TRUE;
        entry->ssid = *ssid;
        entry->channel = channel;
        entry->age_ticks = rt_tick_get();
    }

    if (locked)
    {
        wifi_unlock();
    }
}

/**
 * @brief Look up a cached channel for an SSID
 *
 * @return the cached channel, or 0 when there is no usable entry
 */
static uint8_t scan_channel_cache_lookup(const whd_ssid_t *ssid)
{
    rt_int32_t i;
    uint8_t    channel = 0;
    rt_bool_t  locked = RT_FALSE;

    if (ssid == RT_NULL)
        return 0;

    /* See scan_channel_cache_store(): the cache is shared between the scan
     * callback thread and the thread that starts the scan. */
    if (wifi_lock() == RT_EOK)
    {
        locked = RT_TRUE;
    }

    for (i = 0; i < WHD_SCAN_CHANNEL_CACHE_ENTRIES; i++)
    {
        scan_channel_cache_t *entry = &scan_channel_cache_tab[i];

        if (!entry->valid)
            continue;

        if (entry->ssid.length != ssid->length ||
            memcmp(entry->ssid.value, ssid->value, ssid->length) != 0)
            continue;

        /* Drop an entry that is too old. */
        if (scan_ticks_elapsed(entry->age_ticks) > (rt_int32_t)scan_ms_to_ticks(WHD_SCAN_CHANNEL_CACHE_TTL_MS))
        {
            entry->valid = RT_FALSE;
            channel = 0;
            break;
        }

        channel = entry->channel;
        break;
    }

    if (locked)
    {
        wifi_unlock();
    }

    return channel;
}

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
/**
 * @brief Run one managed scan step and wait for it
 *
 * Starts a scan and waits until it completes. The wait is done in short
 * slices so that the early stop of the join-path scan can be evaluated:
 * once the target AP was seen and the grace period expired, the scan is
 * aborted instead of waiting for the firmware to sweep the remaining
 * channels.
 *
 * @param wifi          WiFi instance
 * @param ssid_ptr      SSID filter (may be RT_NULL)
 * @param ext           Extended scan parameters (may be RT_NULL)
 * @param chan_list     Zero terminated channel list (may be RT_NULL)
 * @param seq_start     Start tick of the whole scan sequence
 * @param budget_ticks  Time budget of the whole scan sequence
 *
 * @return WHD_SUCCESS if the scan was started and awaited, an error code
 *         otherwise
 */
static whd_result_t scan_run_managed_step(drv_wifi_t *wifi,
                                           const whd_ssid_t *ssid_ptr,
                                           const whd_scan_extended_params_t *ext,
                                           const uint16_t *chan_list,
                                           rt_tick_t seq_start,
                                           rt_int32_t budget_ticks)
{
    whd_result_t ret;
    rt_tick_t    grace_ticks;

    grace_ticks = scan_ms_to_ticks(WHD_FAST_SCAN_EARLY_STOP_GRACE_MS);

    ret = whd_wifi_scan(wifi->whd_itf, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY,
                         ssid_ptr, NULL, chan_list, ext,
                         scan_callback,
                         &scan_ctx.scan_result, &scan_ctx);
    if (ret != WHD_SUCCESS)
    {
        return ret;
    }

    /* Wait for the scan, in slices, so that the early stop can be
     * evaluated while the scan is still running. */
    while (scan_ctx.scan_complete_sem != RT_NULL)
    {
        rt_int32_t remaining_ticks;
        rt_int32_t slice_ticks;
        rt_bool_t  target_found;
        rt_tick_t  found_tick;

        remaining_ticks = (rt_int32_t)budget_ticks - scan_ticks_elapsed(seq_start);
        if (remaining_ticks <= 0)
        {
            /* The time budget of the whole sequence is used up. */
            break;
        }

        slice_ticks = (rt_int32_t)grace_ticks;
        if (slice_ticks <= 0)
            slice_ticks = 1;

        if (slice_ticks > remaining_ticks)
            slice_ticks = remaining_ticks;

        if (rt_sem_take(scan_ctx.scan_complete_sem, slice_ticks) == RT_EOK)
        {
            /* The scan finished (or was aborted). */
            break;
        }

        /* The slice expired without the scan finishing. Check whether the
         * early stop may be applied. */
        if (wifi_lock() == RT_EOK)
        {
            target_found = scan_ctx.target_found;
            found_tick   = scan_ctx.target_found_tick;
            wifi_unlock();
        }
        else
        {
            target_found = scan_ctx.target_found;
            found_tick   = scan_ctx.target_found_tick;
        }

        if (!target_found)
            continue;

#if WHD_FAST_SCAN_EARLY_STOP_ENABLE
        /* The target AP was seen. Give it a short grace period, so that a
         * stronger BSS of the same SSID that is reported a bit later can
         * still win. After the grace period the scan is aborted. */
        if (scan_ticks_elapsed(found_tick) < (rt_int32_t)grace_ticks)
            continue;

        LOG_I("Join scan: target AP found, stopping the scan early");

        if (wifi_lock() == RT_EOK)
        {
            scan_ctx.early_stop_requested = RT_TRUE;
            wifi_unlock();
        }
        else
        {
            scan_ctx.early_stop_requested = RT_TRUE;
        }

        /* Abort the scan in the firmware. The scan callback is still
         * invoked with WHD_SCAN_ABORTED, so the event ordering stays the
         * same. */
        whd_wifi_stop_scan(wifi->whd_itf);

        /* Wait for the abort to be reported by the callback. The wait is
         * bounded, so that a lost abort callback cannot stall the join. */
        remaining_ticks = (rt_int32_t)budget_ticks - scan_ticks_elapsed(seq_start);
        if (remaining_ticks > scan_ms_to_ticks(WHD_FAST_SCAN_EARLY_STOP_ABORT_TIMEOUT_MS))
        {
            remaining_ticks = scan_ms_to_ticks(WHD_FAST_SCAN_EARLY_STOP_ABORT_TIMEOUT_MS);
        }
        if (remaining_ticks > 0)
        {
            if (rt_sem_take(scan_ctx.scan_complete_sem, remaining_ticks) != RT_EOK)
            {
                LOG_W("Join scan: the scan abort was not reported in time");
            }
        }
        break;
#else
        /* The early stop is disabled: the scan runs over the whole band. */
        (void)found_tick;
        (void)grace_ticks;
        continue;
#endif
    }

    return WHD_SUCCESS;
}
#endif /* WHD_FAST_JOIN_SCAN_ENABLE */

static rt_err_t drv_wlan_scan(struct rt_wlan_device *wlan, struct rt_scan_info *scan_info)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_ssid_t *ssid_ptr = NULL;
    whd_ssid_t ssid = {0};
    whd_result_t ret;
    rt_bool_t use_fast_scan = RT_FALSE;
    whd_scan_extended_params_t fast_ext;
    rt_bool_t ext_valid = RT_FALSE;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    if (wifi_lock() != RT_EOK)
        return -RT_ERROR;

    if (scan_ctx.is_scanning)
    {
        wifi_unlock();
        return -RT_EBUSY;
    }

    /* Initialize scan context */
    scan_ctx.wlan = wlan;
    scan_ctx.scan_type = WHD_SCAN_TYPE_ACTIVE;
    scan_ctx.is_scanning = RT_TRUE;
    scan_ctx.stop_requested = RT_FALSE;
    scan_ctx.run_passive = RT_TRUE;
    scan_ctx.managed = RT_FALSE;
    scan_ctx.stage = WHD_SCAN_STAGE_IDLE;
    scan_ctx.target_found = RT_FALSE;
    scan_ctx.early_stop_requested = RT_FALSE;
    scan_ctx.target_found_tick = rt_tick_get();
    scan_ctx.has_target_ssid = RT_FALSE;
    scan_ctx.done_reported = RT_FALSE;
    memset(&scan_ctx.target_ssid, 0, sizeof(scan_ctx.target_ssid));
    memset(&scan_ctx.scan_result, 0, sizeof(scan_ctx.scan_result));

    /* The scan completion semaphore is created once and kept for the
     * lifetime of the driver. It must not be deleted while a late scan
     * callback may still release it. */
    if (scan_ctx.scan_complete_sem == RT_NULL)
    {
        scan_ctx.scan_complete_sem = rt_sem_create("scan_done", 0, RT_IPC_FLAG_PRIO);
        if (scan_ctx.scan_complete_sem == RT_NULL)
        {
            scan_ctx.is_scanning = RT_FALSE;
            wifi_unlock();
            return -RT_ERROR;
        }
    }
    else
    {
        /* Remove the tokens of a previous scan, so that the wait below
         * really waits for this scan. */
        scan_sem_drain(scan_ctx.scan_complete_sem);
    }

    /* Set SSID filter if provided */
    if (scan_info != NULL && scan_info->ssid.len > 0)
    {
        rt_uint8_t ssid_filter_len = (rt_uint8_t)scan_info->ssid.len;
        if (ssid_filter_len > sizeof(ssid.value))
            ssid_filter_len = sizeof(ssid.value);
        if (ssid_filter_len > RT_WLAN_SSID_MAX_LENGTH)
            ssid_filter_len = RT_WLAN_SSID_MAX_LENGTH;

        ssid.length = ssid_filter_len;
        memcpy(ssid.value, scan_info->ssid.val, ssid_filter_len);
        ssid_ptr = &ssid;

        /* Join-path scans (SSID-filtered) should be active-only.
         * Running an immediate passive scan after an active scan can leave
         * the firmware in a transient state that destabilizes the join. */
        scan_ctx.run_passive = RT_FALSE;

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
        /* This is the join-path scan: only the requested AP has to be
         * found, so the scan may use a short per-channel dwell time. The
         * driver supervises the scan itself so that it can repeat it with
         * the firmware default dwell times if the AP was not found. */
        use_fast_scan = RT_TRUE;
        scan_ctx.managed = RT_TRUE;
        scan_ctx.stage = WHD_SCAN_STAGE_FAST;
        scan_ctx.target_ssid = ssid;
        scan_ctx.has_target_ssid = RT_TRUE;

        memset(&fast_ext, 0, sizeof(fast_ext));
        fast_ext.number_of_probes_per_channel = WHD_FAST_SCAN_PROBES;
        fast_ext.scan_active_dwell_time_per_channel_ms = WHD_FAST_SCAN_ACTIVE_DWELL_MS;
        fast_ext.scan_passive_dwell_time_per_channel_ms = WHD_FAST_SCAN_PASSIVE_DWELL_MS;
        fast_ext.scan_home_channel_dwell_time_between_channels_ms = WHD_FAST_SCAN_HOME_DWELL_MS;
        ext_valid = RT_TRUE;
#endif
    }

    /* Do not hold wifi_mutex while calling into WHD. WHD may invoke scan_callback()
     * from within whd_wifi_scan() or its worker thread, and scan_callback() also
     * touches scan_ctx under wifi_mutex. */
    wifi_unlock();

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
    if (use_fast_scan)
    {
        rt_tick_t seq_start;
        rt_int32_t budget_ticks;
        uint16_t cached_chan_list[2];
        const uint16_t *chan_list = RT_NULL;

        budget_ticks = (rt_int32_t)scan_ms_to_ticks(WHD_FAST_SCAN_SEQUENCE_BUDGET_MS);
        seq_start = rt_tick_get();

        /* ---- Step 1: single channel scan on the cached channel ----
         *
         * When the channel of the target SSID is known from a previous scan,
         * the scan is restricted to that channel. The cached channel is only
         * a hint: if the AP is not found on it, the full band scan (step 2)
         * is started as usual. */
        {
            uint8_t cached_ch = scan_channel_cache_lookup(&ssid);

            if (cached_ch != 0)
            {
                /* The channel list is terminated by a zero entry. The
                 * values are the raw channel numbers, whd_wifi_scan()
                 * converts them into chanspecs. */
                cached_chan_list[0] = (uint16_t)cached_ch;
                cached_chan_list[1] = 0;
                chan_list = cached_chan_list;

                LOG_I("Join scan: trying cached channel %u for ssid %s",
                      (unsigned int)cached_ch, (char *)ssid.value);
            }
        }

        /* ---- Step 2: fast scan over the whole band ----
         *
         * Only the requested AP has to be found, so the scan may use a
         * short per-channel dwell time. This step is skipped when the
         * single channel scan of step 1 already found the AP. */
        if (chan_list != RT_NULL)
        {
            ret = scan_run_managed_step(wifi, ssid_ptr,
                                        (ext_valid ? &fast_ext : RT_NULL),
                                        chan_list, seq_start, budget_ticks);
            if (ret != WHD_SUCCESS)
            {
                LOG_W("Cached channel scan failed to start: %lu", (unsigned long)ret);
            }        }

        if (!scan_ctx.stop_requested && !scan_ctx.target_found)
        {
            /* The cached channel did not hold the AP (or there was no
             * cached channel). Scan the whole band. */
            if (chan_list != RT_NULL)
            {
                if (wifi_lock() == RT_EOK)
                {
                    scan_ctx.stage = WHD_SCAN_STAGE_FALLBACK;
                    scan_ctx.target_found = RT_FALSE;
                    wifi_unlock();
                }
                else
                {
                    scan_ctx.stage = WHD_SCAN_STAGE_FALLBACK;
                    scan_ctx.target_found = RT_FALSE;
                }

                /* The scan may have released the completion semaphore more
                 * than once (an aborted scan produces an extra callback).
                 * Remove all pending tokens so that the wait really waits
                 * for the next scan. */
                scan_sem_drain(scan_ctx.scan_complete_sem);
            }

            ret = scan_run_managed_step(wifi, ssid_ptr,
                                        (ext_valid ? &fast_ext : RT_NULL),
                                        RT_NULL, seq_start, budget_ticks);
            if (ret != WHD_SUCCESS)
            {
                LOG_W("Fast scan failed to start: %lu", (unsigned long)ret);
            }
        }

        /* The fast scan may have missed the AP (busy channel, hidden SSID,
         * weak signal, or a probe that was lost). In that case repeat the
         * scan with the firmware default dwell times, so that the behaviour
         * is identical to a scan without the fast path.
         *
         * The SCAN_DONE event is reported exactly once, at the end of the
         * whole scan sequence, by this thread. */
        if (!scan_ctx.stop_requested && !scan_ctx.target_found)
        {
            rt_int32_t elapsed_ticks;
            rt_int32_t remaining_ticks;

            elapsed_ticks = scan_ticks_elapsed(seq_start);
            remaining_ticks = budget_ticks - elapsed_ticks;
            if (remaining_ticks <= 0)
            {
                /* The time budget for the whole scan sequence is used up.
                 * Do not start the fallback scan, otherwise the SCAN_DONE
                 * event would arrive after the framework gave up waiting
                 * for it. */
                LOG_W("Fast scan time budget exhausted, skipping the fallback scan");
            }
            else
            {
                LOG_I("Fast scan did not find the target AP, retrying with default scan parameters");

                if (wifi_lock() == RT_EOK)
                {
                    scan_ctx.scan_type = WHD_SCAN_TYPE_ACTIVE;
                    scan_ctx.stage = WHD_SCAN_STAGE_FALLBACK_RUN;
                    scan_ctx.target_found = RT_FALSE;
                    memset(&scan_ctx.scan_result, 0, sizeof(scan_ctx.scan_result));
                    wifi_unlock();
                }

                /* The fast scan may have released the completion semaphore
                 * more than once (an aborted scan produces an extra callback).
                 * Remove all pending tokens so that the wait below really waits
                 * for the fallback scan. */
                scan_sem_drain(scan_ctx.scan_complete_sem);

                ret = whd_wifi_scan(wifi->whd_itf, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY,
                                    ssid_ptr, NULL, NULL, NULL, scan_callback,
                                    &scan_ctx.scan_result, &scan_ctx);
                if (ret != WHD_SUCCESS)
                {
                    /* The fallback scan could not be started, so no scan
                     * callback will arrive for it. */
                    LOG_W("Fallback scan failed to start: %lu", ret);
                }
                else
                {
                    /* Wait for the fallback scan to complete. The scan
                     * callback only updates the state and wakes this thread
                     * up, the SCAN_DONE event is reported below. */
                    if (scan_ctx.scan_complete_sem != RT_NULL)
                    {
                        if (rt_sem_take(scan_ctx.scan_complete_sem, remaining_ticks) != RT_EOK)
                        {
                            LOG_W("Fallback scan did not finish within the time budget");
                        }
                    }
                }
            }
        }

        /* The scan sequence is over. Close it and report SCAN_DONE
         * exactly once. */
        {
            rt_bool_t report_done = RT_FALSE;

            if (wifi_lock() == RT_EOK)
            {
                scan_ctx.is_scanning = RT_FALSE;
                scan_ctx.managed = RT_FALSE;
                scan_ctx.has_target_ssid = RT_FALSE;
                scan_ctx.stage = WHD_SCAN_STAGE_DONE;
                if (!scan_ctx.done_reported)
                {
                    scan_ctx.done_reported = RT_TRUE;
                    report_done = RT_TRUE;
                }
                wifi_unlock();
            }
            else
            {
                scan_ctx.is_scanning = RT_FALSE;
                scan_ctx.managed = RT_FALSE;
                scan_ctx.has_target_ssid = RT_FALSE;
                scan_ctx.stage = WHD_SCAN_STAGE_DONE;
                if (!scan_ctx.done_reported)
                {
                    scan_ctx.done_reported = RT_TRUE;
                    report_done = RT_TRUE;
                }
            }

            if (report_done)
            {
                rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL);
            }
        }
    }
    else
#endif
    {
        /* Start the scan. This is the generic user scan path (no SSID
         * filter), so the firmware default scan parameters are used. */
        ret = whd_wifi_scan(wifi->whd_itf, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY,
                            ssid_ptr, NULL, NULL,
                            (ext_valid ? &fast_ext : NULL),
                            scan_callback,
                            &scan_ctx.scan_result, &scan_ctx);

        if (ret != WHD_SUCCESS)
        {
            LOG_E("Failed to start scan: %lu", ret);
            /* The semaphore is kept alive for the lifetime of the driver, so
             * that a late scan callback cannot release a deleted object. */
            if (wifi_lock() == RT_EOK)
            {
                scan_ctx.is_scanning = RT_FALSE;
                scan_ctx.managed = RT_FALSE;
                scan_ctx.has_target_ssid = RT_FALSE;
                scan_ctx.stage = WHD_SCAN_STAGE_DONE;
                wifi_unlock();
            }
            else
            {
                scan_ctx.is_scanning = RT_FALSE;
                scan_ctx.managed = RT_FALSE;
                scan_ctx.has_target_ssid = RT_FALSE;
                scan_ctx.stage = WHD_SCAN_STAGE_DONE;
            }
            return -RT_ERROR;
        }

        /* For generic user scans, wait for active scan completion then start passive scan.
         * For join-path SSID scans, active-only mode skips passive scan entirely. */
        if (scan_ctx.run_passive &&
            scan_ctx.scan_complete_sem != RT_NULL &&
            rt_sem_take(scan_ctx.scan_complete_sem, rt_tick_from_millisecond(SCAN_TIMEOUT_MS)) == RT_EOK)
        {
            if (!scan_ctx.stop_requested)
            {
                /* Start passive scan */
                ret = whd_wifi_scan(wifi->whd_itf, WHD_SCAN_TYPE_PASSIVE, WHD_BSS_TYPE_ANY,
                                    ssid_ptr, NULL, NULL, NULL, scan_callback,
                                    &scan_ctx.scan_result, &scan_ctx);

                if (ret != WHD_SUCCESS)
                {
                    LOG_W("Passive scan failed: %lu", ret);
                }
            }
        }
    }

    /* The scan completion semaphore is not deleted here. It lives as
     * long as the driver, so that a late scan callback cannot release a
     * deleted object. */

    return RT_EOK;
}

static rt_err_t drv_wlan_scan_stop(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    rt_bool_t need_stop = RT_FALSE;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    if (wifi_lock() == RT_EOK)
    {
        if (scan_ctx.is_scanning)
        {
            scan_ctx.stop_requested = RT_TRUE;
            need_stop = RT_TRUE;
        }
        wifi_unlock();
    }
    else
    {
        if (scan_ctx.is_scanning)
        {
            scan_ctx.stop_requested = RT_TRUE;
            need_stop = RT_TRUE;
        }
    }

    if (!need_stop)
        return RT_EOK;

    whd_wifi_stop_scan(wifi->whd_itf);

    return RT_EOK;
}

/*******************************************************************************
 * Pre-join AP Probe Scan
 ******************************************************************************/

typedef struct {
    whd_scan_result_t result;
    rt_sem_t          sem;
    rt_bool_t         found;
    const whd_ssid_t *target_ssid;
} probe_scan_ctx_t;

/* The probe scan context and the scan result buffer are static objects.
 *
 * WHD keeps the pointers to them (the callback user data and the result
 * pointer) in its internal state until the scan ends. A scan that is
 * aborted or that ends late could therefore still deliver a callback after
 * wifi_probe_ap() returned. Static objects and a semaphore that is never
 * deleted avoid a use-after-free in that case. */
static probe_scan_ctx_t  probe_ctx;
static whd_scan_result_t probe_scan_buf;
static rt_sem_t          probe_sem;

static void probe_scan_callback(whd_scan_result_t **result_ptr,
                                void *user_data,
                                whd_scan_status_t status)
{
    probe_scan_ctx_t *ctx = (probe_scan_ctx_t *)user_data;

    if (ctx == NULL)
        return;

    /* Scan complete / aborted - release waiter */
    if (result_ptr == NULL || *result_ptr == NULL)
    {
        if (ctx->sem != RT_NULL)
            rt_sem_release(ctx->sem);
        return;
    }

    /* Capture first matching result */
    if (!ctx->found)
    {
        whd_scan_result_t *r = *result_ptr;
        if (ctx->target_ssid != NULL &&
            r->SSID.length == ctx->target_ssid->length &&
            memcmp(r->SSID.value, ctx->target_ssid->value, r->SSID.length) == 0)
        {
            memcpy(&ctx->result, r, sizeof(whd_scan_result_t));
            ctx->found = RT_TRUE;

            /* Wake up waiter immediately - no need to wait for full scan.
             * wifi_probe_ap() will call whd_wifi_stop_scan() after waking. */
            if (ctx->sem != RT_NULL)
                rt_sem_release(ctx->sem);
        }
    }
}

/**
 * @brief Quick targeted scan to detect AP parameters before join.
 *
 * Sends probe requests for the target SSID and captures the first
 * matching AP's security type, BSSID, and channel. The scan also brings the
 * firmware RF path up before the join attempt.
 *
 * @param ifp         WHD interface
 * @param ssid        Target SSID to probe
 * @param out_result  Output: AP information if found
 * @param timeout_ms  Maximum wait time (typically 3000)
 * @return RT_EOK if AP found, error otherwise
 */
static rt_err_t wifi_probe_ap(whd_interface_t ifp, const whd_ssid_t *ssid,
                              whd_scan_result_t *out_result, uint32_t timeout_ms)
{
    probe_scan_ctx_t *ctx = &probe_ctx;
    whd_result_t ret;
#ifdef WHD_FAST_JOIN_SCAN_ENABLE
    whd_scan_extended_params_t fast_ext;
    rt_bool_t use_fast = RT_TRUE;
    rt_tick_t probe_start;
    rt_tick_t probe_budget;
#endif

    memset(ctx, 0, sizeof(*ctx));
    memset(&probe_scan_buf, 0, sizeof(probe_scan_buf));

    ctx->target_ssid = ssid;
    ctx->found       = RT_FALSE;

    /* The semaphore is created once and kept for the lifetime of the
     * driver. It must not be deleted while a late scan callback may
     * still release it. */
    if (probe_sem == RT_NULL)
    {
        probe_sem = rt_sem_create("probe", 0, RT_IPC_FLAG_PRIO);
        if (probe_sem == RT_NULL)
            return -RT_ENOMEM;
    }
    ctx->sem = probe_sem;
    scan_sem_drain(ctx->sem);

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
    /* Short per-channel dwell time: this probe scan only looks for one
     * specific SSID, so a fast scan is enough in the common case. */
    memset(&fast_ext, 0, sizeof(fast_ext));
    fast_ext.number_of_probes_per_channel = WHD_FAST_SCAN_PROBES;
    fast_ext.scan_active_dwell_time_per_channel_ms = WHD_FAST_SCAN_ACTIVE_DWELL_MS;
    fast_ext.scan_passive_dwell_time_per_channel_ms = WHD_FAST_SCAN_PASSIVE_DWELL_MS;
    fast_ext.scan_home_channel_dwell_time_between_channels_ms = WHD_FAST_SCAN_HOME_DWELL_MS;

    /* The whole probe sequence (fast scan plus optional fallback scan)
     * has to stay inside the given timeout. */
    probe_budget = scan_ms_to_ticks((rt_int32_t)timeout_ms);
    probe_start = rt_tick_get();
#endif

    for (;;)
    {
        rt_tick_t wait_ticks = rt_tick_from_millisecond(timeout_ms);

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
        /* Limit the wait of the current scan to the remaining time
         * budget. */
        {
            rt_int32_t remaining_ticks;

            remaining_ticks = (rt_int32_t)probe_budget - scan_ticks_elapsed(probe_start);
            if (remaining_ticks <= 0)
            {
                LOG_W("Probe scan time budget exhausted");
                break;
            }
            wait_ticks = (rt_tick_t)remaining_ticks;
        }
#endif

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
        ret = whd_wifi_scan(ifp, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY,
                            ssid, NULL, NULL,
                            (use_fast ? &fast_ext : NULL),
                            probe_scan_callback, &probe_scan_buf, ctx);
#else
        ret = whd_wifi_scan(ifp, WHD_SCAN_TYPE_ACTIVE, WHD_BSS_TYPE_ANY,
                            ssid, NULL, NULL, NULL,
                            probe_scan_callback, &probe_scan_buf, ctx);
#endif
        if (ret != WHD_SUCCESS)
        {
            LOG_D("Probe scan start failed: 0x%lx", (unsigned long)ret);
            return -RT_ERROR;
        }

        /* Wait for scan to finish (or timeout) */
        rt_sem_take(ctx->sem, wait_ticks);
        whd_wifi_stop_scan(ifp);

        /* Brief settle time for firmware scan cleanup */
        rt_thread_mdelay(10);

        if (ctx->found)
            break;

#ifdef WHD_FAST_JOIN_SCAN_ENABLE
        /* The fast scan did not find the AP. Repeat it with the firmware
         * default dwell times so that the probe result is exactly the same
         * as without the fast path. */
        if (use_fast)
        {
            LOG_I("Fast probe scan did not find the target AP, retrying with default scan parameters");
            use_fast = RT_FALSE;
            ctx->found = RT_FALSE;
            /* Remove all pending tokens of the completion semaphore (an
             * aborted scan produces an extra callback), so that the wait
             * below really waits for the fallback scan. */
            scan_sem_drain(ctx->sem);
            continue;
        }
#endif
        break;
    }

    /* The semaphore is not deleted here, it lives as long as the driver. */
    ctx->sem = RT_NULL;

    if (ctx->found)
    {
        *out_result = ctx->result;
        return RT_EOK;
    }

    return -RT_ERROR;
}

/**
 * @brief Internal connect function
 */
static cy_rslt_t wifi_connect_internal(drv_wifi_t *wifi, const connect_params_t *params)
{
    whd_result_t ret;
    uint32_t ext_sae_support = 0;
    whd_security_t join_security;  /* Effective security for this join */

    if (wifi == NULL || wifi->whd_itf == NULL || params == NULL)
        return CY_RSLT_TYPE_ERROR;


    join_security = params->security;
  /*  if (join_security == WHD_SECURITY_WPA3_WPA2_PSK)
    {
        LOG_I("WPA3/WPA2 transition mode: using WPA3-SAE for connection");
    }*/

    /* Store current security type for event handling */
    sta_security_type = join_security;

    /* Reset state flags */
    link_up_event_received = RT_FALSE;


    whd_wifi_leave(wifi->whd_itf);

    wifi->state = WIFI_STATE_CONNECTING;


    whd_wifi_deregister_event_handler(wifi->whd_itf, wifi->event_handler_index);

    /* Explicitly stop any prior scan before join. */
    whd_wifi_stop_scan(wifi->whd_itf);


    if (join_security != WHD_SECURITY_WPA3_SAE &&
        join_security != WHD_SECURITY_WPA3_WPA2_PSK)
    {
        /* WPA2/Open: disable MFP to avoid the PMF cached-SA issue */
        wifi->whd_itf->whd_driver->is_mfp_auto = true;
        whd_wifi_set_iovar_value(wifi->whd_itf, IOVAR_STR_MFP, WL_MFP_NONE);
    }
    else
    {
        /* WPA3-SAE / WPA3_WPA2_PSK: MFP is REQUIRED per spec */
        wifi->whd_itf->whd_driver->is_mfp_auto = false;
    }

    whd_wifi_get_fwcap(wifi->whd_itf, &ext_sae_support);
    LOG_I("fwcap=0x%08lX, SAE_EXT bit(%d)=%s",
          (unsigned long)ext_sae_support, (int)WHD_FWCAP_SAE_EXT,
          (ext_sae_support & (1 << WHD_FWCAP_SAE_EXT)) ? "YES" : "NO");

    if ((ext_sae_support & (1 << WHD_FWCAP_SAE_EXT)) &&
        (join_security == WHD_SECURITY_WPA3_SAE ||
         join_security == WHD_SECURITY_WPA3_WPA2_PSK))
    {
#ifdef WHD_USING_WPA3
        LOG_I("[WPA3] AP security=%d, SAE_EXT supported -> starting SAE supplicant", join_security);
        ret = wpa3_supplicant_sae_start((uint8_t *)params->ssid.value, params->ssid.length,
                                         (uint8_t *)params->key, params->key_len);
        if (ret != CY_RSLT_SUCCESS)
        {
            LOG_E("WPA3 SAE supplicant start failed: 0x%08lX", (unsigned long)ret);
            wifi->state = WIFI_STATE_IDLE;
            return ret;
        }
#else
        LOG_E("WPA3 not enabled: enable WHD_USING_WPA3 in Kconfig");
        wifi->state = WIFI_STATE_IDLE;
        return CY_RSLT_TYPE_ERROR;
#endif
    }

    /* ---- Join ----
     *
     * When channel+BSSID are known from the probe scan or the management
     * layer, whd_wifi_join_specific() is used and the ~3s full-channel scan
     * of whd_wifi_join() is avoided. Otherwise fall back to whd_wifi_join()
     * and let the firmware do the discovery.
     */
    whd_wifi_set_ioctl_value(wifi->whd_itf, WLC_SET_BAND, WLC_BAND_AUTO);

    if (params->channel != 0 &&
        mac_is_nonzero(params->bssid.octet, sizeof(params->bssid.octet)))
    {
        /* Build whd_scan_result_t from probed AP info for join_specific */
        whd_scan_result_t ap_target;
        memset(&ap_target, 0, sizeof(ap_target));
        memcpy(&ap_target.SSID, &params->ssid, sizeof(whd_ssid_t));
        memcpy(&ap_target.BSSID, &params->bssid, sizeof(whd_mac_t));
        ap_target.security = join_security;
        ap_target.channel  = params->channel;
        ap_target.band     = (params->channel <= 14) ? WHD_802_11_BAND_2_4GHZ
                                                     : WHD_802_11_BAND_5GHZ;
        ap_target.bss_type = WHD_BSS_TYPE_INFRASTRUCTURE;

        LOG_D("join_specific: ch=%u BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
              ap_target.channel,
              ap_target.BSSID.octet[0], ap_target.BSSID.octet[1],
              ap_target.BSSID.octet[2], ap_target.BSSID.octet[3],
              ap_target.BSSID.octet[4], ap_target.BSSID.octet[5]);

        ret = whd_wifi_join_specific(wifi->whd_itf, &ap_target,
                                     params->key, params->key_len);
    }
    else
    {
        /* No channel/BSSID info - let firmware do full-channel discovery */
        ret = whd_wifi_join(wifi->whd_itf, &params->ssid, join_security,
                            params->key, params->key_len);
    }

    /* ---- Re-register STA event handler after join ----
     * whd_wifi_join() is synchronous - when it returns, the join is either
     * fully complete (auth+assoc+handshake) or has failed and WHD has
     * already called whd_wifi_leave() internally.
     *
 * Re-registering sends the updated event mask to firmware so that later
 * link-down / deauth events are received.
     */
    whd_wifi_set_event_handler(wifi->whd_itf, sta_link_events,
                               (whd_event_handler_t)sta_event_handler,
                               wifi, &wifi->event_handler_index);

    if (ret != WHD_SUCCESS)
    {
        LOG_I("[DIAG] WiFi join attempt failed: 0x%08lX (sec=%d, ch=%u)", (unsigned long)ret,
              (int)join_security, (unsigned)params->channel);
        wifi->state = WIFI_STATE_IDLE;
        return ret;
    }

    /* Join succeeded - whd_wifi_join() only returns success when the
     * firmware has completed authentication, association, and (for WPA)
     * the 4-way handshake. Notify the framework now.
     */
    LOG_D("[DIAG] WiFi join OK (sec=%d, ch=%u)", (int)join_security, (unsigned)params->channel);
    link_up_handler(wifi);

    return CY_RSLT_SUCCESS;
}

static rt_err_t drv_wlan_join(struct rt_wlan_device *wlan, struct rt_sta_info *sta_info)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    connect_params_t params = {0};
    cy_rslt_t ret;

    if (wifi == NULL || wifi->whd_itf == NULL || sta_info == NULL)
        return -RT_EINVAL;

    /* Clear disconnect protection flag - user is actively trying to connect.
     * Keep lock region short; don't hold it across WHD join operations. */
    if (wifi_lock() == RT_EOK)
    {
        is_disconnect_triggered = RT_FALSE;
        wifi_unlock();
    }
    else
    {
        is_disconnect_triggered = RT_FALSE;
    }

    /* Store connection parameters (with bounds checks to avoid memory corruption) */
    if (sta_info->ssid.len > sizeof(params.ssid.value))
    {
        LOG_E("SSID too long: %d", sta_info->ssid.len);
        return -RT_EINVAL;
    }
    params.ssid.length = sta_info->ssid.len;
    if (params.ssid.length > 0)
    {
        memcpy(params.ssid.value, sta_info->ssid.val, params.ssid.length);
    }

    if (mac_is_nonzero(sta_info->bssid, sizeof(sta_info->bssid)))
    {
        memcpy(params.bssid.octet, sta_info->bssid, sizeof(params.bssid.octet));
    }

    params.security = rtt_to_whd_security(sta_info->security);

    if (sta_info->key.len > sizeof(params.key))
    {
        LOG_E("Key too long: %d", sta_info->key.len);
        return -RT_EINVAL;
    }
    params.key_len = sta_info->key.len;
    if (params.key_len > 0)
    {
        memcpy(params.key, sta_info->key.val, params.key_len);
    }

    /* With RT_WLAN_JOIN_SCAN_BY_MGNT disabled, CLI join may provide unknown
     * security. Probe the AP to get exact security type, BSSID, and
     * channel. The scan also brings the firmware RF path up before join.
     *
     * If probe fails (AP not found / timeout) fall back to a safe default.
     *
     * WPA3-SAE: skip probe scan - the SAE supplicant's internal scan and
     * the firmware's join scan handle AP discovery. Adding a probe scan
     * wastes ~3s and creates additional firmware scan state transitions
     * that can interfere with the ext-auth flow.
     */
    if (params.security == WHD_SECURITY_UNKNOWN ||
        !mac_is_nonzero(params.bssid.octet, sizeof(params.bssid.octet)))
    {
#ifdef WHD_USING_WPA3
        /* For known WPA3 security, skip the probe scan entirely */
        if (params.security == WHD_SECURITY_WPA3_SAE ||
            params.security == WHD_SECURITY_WPA3_WPA2_PSK)
        {
            LOG_D("WPA3: skip probe scan (SAE supplicant + FW do own scan)");
        }
        else
#endif
        {
            whd_scan_result_t ap_info;
            if (wifi_probe_ap(wifi->whd_itf, &params.ssid, &ap_info, 3000) == RT_EOK)
            {
                /* ap_info.security is the raw union-of-flags bitmask built by
                 * the driver, e.g. 0x45400004 for a WPA2/WPA3 mixed AP that
                 * also advertises SHA-256 and 802.11r. It must be normalized
                 * to a canonical whd_security_t value before it is used as
                 * the join security: whd_wifi_prepare_join() switches on the
                 * whole value and would otherwise hit its "Unsupported
                 * security type" assert, and the WPA3 SAE detection below
                 * would not recognise it as SAE. */
                whd_security_t probed = whd_normalize_security(ap_info.security);
                if (probed == WHD_SECURITY_UNKNOWN)
                {
                    LOG_E("Probed AP security 0x%08lX could not be mapped",
                          (unsigned long)(rt_uint32_t)ap_info.security);
                }
                params.security = probed;
                memcpy(params.bssid.octet, ap_info.BSSID.octet, sizeof(params.bssid.octet));
                params.channel  = ap_info.channel;
                LOG_D("Probed AP: raw_security=0x%08lX security=0x%08lX ch=%lu BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
                      (unsigned long)(rt_uint32_t)ap_info.security,
                      (unsigned long)(rt_uint32_t)params.security,
                      (unsigned long)params.channel,
                      params.bssid.octet[0], params.bssid.octet[1],
                      params.bssid.octet[2], params.bssid.octet[3],
                      params.bssid.octet[4], params.bssid.octet[5]);
            }
            else
            {
                LOG_W("Probe scan failed, using fallback security");
                if (params.security == WHD_SECURITY_UNKNOWN)
                {
                    params.security = (params.key_len > 0) ? WHD_SECURITY_WPA2_AES_PSK
                                                           : WHD_SECURITY_OPEN;
                }
            }
        }
    }

    /* Use management-layer channel if provided, otherwise keep probed value */
    if (sta_info->channel != 0)
        params.channel = sta_info->channel;
    params.is_valid = RT_TRUE;

    /* Attempt connection with retry logic as a safety net against
     * transient RF failures. */
    {
        uint32_t retry;
        for (retry = 0; retry < WHD_JOIN_MAX_RETRIES; retry++)
        {
            ret = wifi_connect_internal(wifi, &params);
            if (ret == CY_RSLT_SUCCESS)
            {
                break;  /* Join initiated successfully */
            }

            /* SDIO-level errors won't recover with a simple retry */
            if (IS_SDIO_ERROR(ret))
            {
                LOG_E("SDIO error during join, aborting retries");
                break;
            }

            LOG_D("WiFi join attempt %lu/%d failed (err=0x%08lX), retrying in %d ms...",
                  retry + 1, WHD_JOIN_MAX_RETRIES, ret, WHD_JOIN_RETRY_DELAY_MS);

            /* WHD already calls whd_wifi_leave() internally on join failure,
             * so only a wait is needed before the next attempt.
             *
             * For WPA3 SAE: the SAE supplicant's background thread needs
             * time to clean up (thread join + resource deallocation) before
             * the next wpa3_supplicant_sae_start() call, so use a longer
             * delay (1s) to ensure the SAE workspace is fully released.
             */
#ifdef WHD_USING_WPA3
            if (params.security == WHD_SECURITY_WPA3_SAE ||
                params.security == WHD_SECURITY_WPA3_WPA2_PSK)
            {
                rt_thread_mdelay(1000);
            }
            else
#endif
            {
                rt_thread_mdelay(WHD_JOIN_RETRY_DELAY_MS);
            }
        }

        if (ret != CY_RSLT_SUCCESS)
        {
            LOG_E("WiFi join failed after %d attempts (last err=0x%08lX)",
                  WHD_JOIN_MAX_RETRIES, ret);
        }
    }

    /* Update connected_ap under lock (shared state) */
    if (wifi_lock() == RT_EOK)
    {
        if (ret == CY_RSLT_SUCCESS)
        {
            /* Store for potential reconnection after successful join initiation */
            connected_ap = params;
        }
        else if (!connected_ap.is_valid)
        {
            /* First-time connection failed, still save params to allow retry */
            connected_ap = params;
        }
        /* else: keep previous successful AP info, don't overwrite */
        wifi_unlock();
    }
    else
    {
        if (ret == CY_RSLT_SUCCESS)
            connected_ap = params;
        else if (!connected_ap.is_valid)
            connected_ap = params;
    }

    if (ret != CY_RSLT_SUCCESS)
    {
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_CONNECT_FAIL, RT_NULL);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t drv_wlan_disconnect(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_result_t ret;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    /* Update shared state first, then call WHD without holding the mutex. */
    if (wifi_lock() == RT_EOK)
    {
        /* Set disconnect flag (prevents auto-reconnect until next successful join) */
        is_disconnect_triggered = RT_TRUE;

        /* Clear auto-reconnect info - permanent until next successful join.
         * Prevents reconnecting to old AP during an intentional disconnect. */
        connected_ap.is_valid = RT_FALSE;

        wifi->state = WIFI_STATE_DISCONNECTING;
        wifi_unlock();
    }
    else
    {
        is_disconnect_triggered = RT_TRUE;
        connected_ap.is_valid = RT_FALSE;
        wifi->state = WIFI_STATE_DISCONNECTING;
    }

    ret = whd_wifi_leave(wifi->whd_itf);

    if (ret == WHD_SUCCESS)
    {
        if (wifi_lock() == RT_EOK)
        {
            sta_link_up = RT_FALSE;
            wifi->state = WIFI_STATE_IDLE;
            wifi_unlock();
        }
        else
        {
            sta_link_up = RT_FALSE;
            wifi->state = WIFI_STATE_IDLE;
        }
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL);

        /* Keep is_disconnect_triggered = TRUE until the next join attempt.
         * The flag is cleared by the join path. */
    }
    else
    {
        /* Disconnect failed, but keep the protection flag set anyway */
    }

    RT_UNUSED(is_disconnect_triggered);

    return (ret == WHD_SUCCESS) ? RT_EOK : -RT_ERROR;
}

static rt_err_t drv_wlan_softap(struct rt_wlan_device *wlan, struct rt_ap_info *ap_info)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_ssid_t ap_ssid;
    whd_result_t ret;
    rt_uint8_t ssid_len;
    rt_bool_t sta_up_snapshot = RT_FALSE;
    uint32_t ap_channel_to_use = 0;

    if (wifi == NULL || wifi->whd_itf == NULL || ap_info == NULL)
        return -RT_EINVAL;

    /* Update shared state, then call WHD without holding the mutex. */
    if (wifi_lock() == RT_EOK)
    {
        wifi->state = WIFI_STATE_AP_STARTING;
        /* Store AP security type for event handling */
        ap_security_type = rtt_to_whd_security(ap_info->security);
        sta_up_snapshot = sta_link_up;
        wifi_unlock();
    }
    else
    {
        wifi->state = WIFI_STATE_AP_STARTING;
        ap_security_type = rtt_to_whd_security(ap_info->security);
        sta_up_snapshot = sta_link_up;
    }
    LOG_D("AP security type: %d", ap_security_type);

    /* Safely copy SSID to whd_ssid_t (avoid pointer cast between different struct layouts) */
    ssid_len = (rt_uint8_t)ap_info->ssid.len;
    if (ssid_len > sizeof(ap_ssid.value))
        ssid_len = sizeof(ap_ssid.value);
    ap_ssid.length = ssid_len;
    if (ssid_len > 0)
        memcpy(ap_ssid.value, ap_info->ssid.val, ssid_len);

    /* AP+STA concurrency often requires AP to run on the same channel as the STA link.
     * When STA is already connected, force AP channel to current STA channel. */
    ap_channel_to_use = (uint32_t)ap_info->channel;
#if 0  /* Channel-following disabled: AP uses its configured channel directly */
    if (sta_up_snapshot && wifi_sta.whd_itf != RT_NULL)
    {
        uint32_t sta_channel = 0;

        if (whd_wifi_get_channel(wifi_sta.whd_itf, &sta_channel) == WHD_SUCCESS && sta_channel > 0)
        {
            if (ap_channel_to_use != sta_channel)
            {
                LOG_I("AP+STA: Force AP channel %lu -> %lu (match STA)", ap_channel_to_use, sta_channel);
            }
            ap_channel_to_use = sta_channel;
        }
        else
        {
            LOG_W("AP+STA: Get STA channel failed, use AP channel %lu", ap_channel_to_use);
        }
    }
#endif
    RT_UNUSED(sta_up_snapshot);

    /* Initialize AP */
    ret = whd_wifi_init_ap(wifi->whd_itf,
                           &ap_ssid,
                           ap_security_type,
                           ap_info->key.val,
                           ap_info->key.len,
                           ap_channel_to_use);

    if (ret != WHD_SUCCESS)
    {
        LOG_E("AP init failed: %lu", ret);

        if (wifi_lock() == RT_EOK)
        {
            wifi->state = WIFI_STATE_IDLE;
            wifi_unlock();
        }
        else
        {
            wifi->state = WIFI_STATE_IDLE;
        }
        return -RT_ERROR;
    }

    /* Start AP */
    ret = whd_wifi_start_ap(wifi->whd_itf);

    if (ret != WHD_SUCCESS)
    {
        LOG_E("AP start failed: %lu", ret);

        if (wifi_lock() == RT_EOK)
        {
            wifi->state = WIFI_STATE_IDLE;
            wifi_unlock();
        }
        else
        {
            wifi->state = WIFI_STATE_IDLE;
        }
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_AP_STOP, RT_NULL);
        return -RT_ERROR;
    }

    /* Disable power save (WLC_SET_PM=0, i.e. PM0/Active) and
     * disable MPC (Minimum Power Consumption) on the AP interface
     * so the radio stays fully clocked for stable iperf throughput. */
    {
        whd_result_t ap_pm_ret, ap_mpc_ret;

        ap_pm_ret = whd_wifi_disable_powersave(wifi->whd_itf);
        if (ap_pm_ret != WHD_SUCCESS)
        {
            LOG_W("AP: Failed to disable powersave: 0x%08lX", (unsigned long)ap_pm_ret);
        }
        else
        {
            LOG_I("AP  whd_wifi_disable_powersave: 0x%08lX", (unsigned long)ap_pm_ret);
        }

        ap_mpc_ret = whd_wifi_set_iovar_value(wifi->whd_itf, "mpc", 0);
        LOG_I("AP  whd_wifi_set_iovar_value(mpc,0): 0x%08lX", (unsigned long)ap_mpc_ret);
    }

    /* Enable AMPDU for better throughput (ignore errors as not all chips support it) */
    if (whd_wifi_set_iovar_value(wifi->whd_itf, "ampdu", 1) == WHD_SUCCESS)
    {
        LOG_D("AP: AMPDU enabled");
    }

    if (wifi_lock() == RT_EOK)
    {
        wifi->state = WIFI_STATE_AP_RUNNING;
        ap_link_up = RT_TRUE;
        wifi_unlock();
    }
    else
    {
        wifi->state = WIFI_STATE_AP_RUNNING;
        ap_link_up = RT_TRUE;
    }
    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_AP_START, RT_NULL);

    LOG_I("AP started successfully");
    return RT_EOK;
}

static rt_err_t drv_wlan_ap_stop(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_result_t ret;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    ret = whd_wifi_stop_ap(wifi->whd_itf);

    if (ret == WHD_SUCCESS)
    {
        if (wifi_lock() == RT_EOK)
        {
            wifi->state = WIFI_STATE_IDLE;
            ap_link_up = RT_FALSE;
            ap_security_type = WHD_SECURITY_UNKNOWN;  /* Reset AP security type */
            wifi_unlock();
        }
        else
        {
            wifi->state = WIFI_STATE_IDLE;
            ap_link_up = RT_FALSE;
            ap_security_type = WHD_SECURITY_UNKNOWN;  /* Reset AP security type */
        }
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_AP_STOP, RT_NULL);
    }

    return (ret == WHD_SUCCESS) ? RT_EOK : -RT_ERROR;
}

static rt_err_t drv_wlan_ap_deauth(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_mac_t whd_mac;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    memcpy(whd_mac.octet, mac, sizeof(whd_mac.octet));

    return (whd_wifi_deauth_sta(wifi->whd_itf, &whd_mac, WHD_DOT11_RC_UNSPECIFIED) == WHD_SUCCESS)
           ? RT_EOK : -RT_ERROR;
}

static int drv_wlan_get_rssi(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    int32_t rssi = 0;

    if (wifi != NULL && wifi->whd_itf != NULL)
    {
        whd_wifi_get_rssi(wifi->whd_itf, &rssi);
    }

    return rssi;
}

static rt_err_t drv_wlan_set_powersave(struct rt_wlan_device *wlan, int level)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_result_t result;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    /* AP+STA concurrency can be sensitive to STA powersave on some firmwares.
     * Keep AP stable by refusing to enable STA powersave while AP is running. */
    if (wlan == wifi_sta.wlan && ap_link_up && level > 0)
    {
        LOG_W("STA powersave blocked while AP is running");
        whd_wifi_disable_powersave(wifi->whd_itf);
        return -RT_ERROR;
    }

    switch (level)
    {
    case NO_POWERSAVE_MODE:   /* 0 - disable powersave */
        result = whd_wifi_disable_powersave(wifi->whd_itf);
        break;
    case PM1_POWERSAVE_MODE:  /* 1 - legacy PS-Poll, lowest power */
        result = whd_wifi_enable_powersave(wifi->whd_itf);
        break;
    case PM2_POWERSAVE_MODE:  /* 2 - high-throughput powersave */
    default:
        result = whd_wifi_enable_powersave_with_throughput(wifi->whd_itf,
                    DEFAULT_PM2_SLEEP_RET_TIME);
        break;
    }

    return (result == WHD_SUCCESS) ? RT_EOK : -RT_ERROR;
}

static int drv_wlan_get_powersave(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    uint32_t mode = 0;

    if (wifi != NULL && wifi->whd_itf != NULL)
    {
        whd_wifi_get_powersave_mode(wifi->whd_itf, &mode);
    }

    return (int)mode;
}

static rt_err_t drv_wlan_set_channel(struct rt_wlan_device *wlan, int channel)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    return (whd_wifi_set_channel(wifi->whd_itf, (uint32_t)channel) == WHD_SUCCESS)
           ? RT_EOK : -RT_ERROR;
}

static int drv_wlan_get_channel(struct rt_wlan_device *wlan)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    uint32_t channel = 0;

    if (wifi != NULL && wifi->whd_itf != NULL)
    {
        whd_wifi_get_channel(wifi->whd_itf, &channel);
    }

    return (int)channel;
}

static rt_country_code_t drv_wlan_get_country(struct rt_wlan_device *wlan)
{
    switch (whd_config.country)
    {
        case WHD_COUNTRY_CHINA:
            return RT_COUNTRY_CHINA;
        case WHD_COUNTRY_UNITED_STATES:
            return RT_COUNTRY_UNITED_STATES;
        default:
            return RT_COUNTRY_UNKNOWN;
    }
}

static rt_err_t drv_wlan_set_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_mac_t whd_mac;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    memcpy(whd_mac.octet, mac, sizeof(whd_mac.octet));

    return (whd_wifi_set_mac_address(wifi->whd_itf, whd_mac) == WHD_SUCCESS)
           ? RT_EOK : -RT_ERROR;
}

static rt_err_t drv_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_mac_t whd_mac;
    whd_result_t ret = WHD_BADARG;  /* Default to error */

    if (wifi == NULL)
        return -RT_ERROR;

    /* For AP, derive MAC from STA MAC */
    if (wlan == wifi_ap.wlan && wifi_sta.whd_itf != NULL)
    {
        ret = whd_wifi_get_mac_address(wifi_sta.whd_itf, &whd_mac);
        if (ret == WHD_SUCCESS)
        {
            /* Toggle locally administered bit for AP */
            whd_mac.octet[0] ^= 0x02;
        }
    }
    else if (wifi->whd_itf != NULL)
    {
        ret = whd_wifi_get_mac_address(wifi->whd_itf, &whd_mac);
    }
    else
    {
        return -RT_ERROR;
    }

    if (ret == WHD_SUCCESS)
    {
        memcpy(mac, whd_mac.octet, RT_WLAN_BSSID_MAX_LENGTH);
    }

    return (ret == WHD_SUCCESS) ? RT_EOK : -RT_ERROR;
}

static int drv_wlan_recv(struct rt_wlan_device *wlan, void *buff, int len)
{
    /* Data is received via cy_network_process_ethernet_data callback */
    return RT_EOK;
}

static int drv_wlan_send(struct rt_wlan_device *wlan, void *buff, int len)
{
    drv_wifi_t *wifi = get_drv_wifi(wlan);
    whd_buffer_t buffer;
    whd_result_t ret;

    if (wifi == NULL || wifi->whd_itf == NULL)
        return -RT_ERROR;

    /* Check if interface is ready */
    if (whd_wifi_is_ready_to_transceive(wifi->whd_itf) != WHD_SUCCESS)
    {
        return -RT_ERROR;
    }

    if (buff == RT_NULL || len <= 0)
        return -RT_EINVAL;

    if (len > (INT_MAX - (int)WHD_LINK_HEADER))
        return -RT_EINVAL;

    /* Allocate buffer with space for link header.
     * Keep the timeout short (10 ms) - a long timeout here blocks the
     * eth_tx thread which in turn blocks the tcpip thread (via
     * rt_completion_wait in ethernetif_linkoutput), preventing TCP ACK
     * processing and causing a deadlock under high throughput. */
    ret = whd_host_buffer_get(wifi->whd_itf->whd_driver, &buffer,
                              WHD_NETWORK_TX, len + WHD_LINK_HEADER, 10);
    if (ret != WHD_SUCCESS)
    {
        LOG_D("Buffer allocation failed");
        return -RT_ERROR;
    }

    /* Reserve space for link header */
    whd_buffer_add_remove_at_front(wifi->whd_itf->whd_driver, &buffer, WHD_LINK_HEADER);

    /* Copy data */
    memcpy(whd_buffer_get_current_piece_data_pointer(wifi->whd_itf->whd_driver, buffer),
           buff, len);

    /* Send packet - WHD takes ownership of `buffer` regardless of success/failure,
     * so whd_buffer_release() must NOT be called after this point. */
    ret = whd_network_send_ethernet_data(wifi->whd_itf, buffer);
    if (ret != WHD_SUCCESS)
    {
        LOG_D("whd_network_send_ethernet_data failed: %lu", (unsigned long)ret);
        return -RT_ERROR;
    }

    return RT_EOK;
}

/* WLAN device operations structure */
static const struct rt_wlan_dev_ops wlan_ops = {
    .wlan_init          = drv_wlan_init,
    .wlan_mode          = drv_wlan_mode,
    .wlan_scan          = drv_wlan_scan,
    .wlan_join          = drv_wlan_join,
    .wlan_softap        = drv_wlan_softap,
    .wlan_disconnect    = drv_wlan_disconnect,
    .wlan_ap_stop       = drv_wlan_ap_stop,
    .wlan_ap_deauth     = drv_wlan_ap_deauth,
    .wlan_scan_stop     = drv_wlan_scan_stop,
    .wlan_get_rssi      = drv_wlan_get_rssi,
    .wlan_set_powersave = drv_wlan_set_powersave,
    .wlan_get_powersave = drv_wlan_get_powersave,
    .wlan_cfg_promisc   = RT_NULL,
    .wlan_cfg_filter    = RT_NULL,
    .wlan_cfg_mgnt_filter = RT_NULL,
    .wlan_set_channel   = drv_wlan_set_channel,
    .wlan_get_channel   = drv_wlan_get_channel,
    .wlan_set_country   = RT_NULL,
    .wlan_get_country   = drv_wlan_get_country,
    .wlan_set_mac       = drv_wlan_set_mac,
    .wlan_get_mac       = drv_wlan_get_mac,
    .wlan_recv          = drv_wlan_recv,
    .wlan_send          = drv_wlan_send,
    .wlan_send_raw_frame = RT_NULL,
    .wlan_get_fast_info = RT_NULL,
    .wlan_fast_connect  = RT_NULL,
};

/*******************************************************************************
 * Initialization
 ******************************************************************************/

/**
 * @brief Register event handlers for WHD events
 */
static cy_rslt_t register_event_handlers(void)
{
    whd_result_t ret;

    if (wifi_sta.whd_itf == NULL)
    {
        LOG_E("STA interface is NULL, cannot register STA events");
        return CY_RSLT_TYPE_ERROR;
    }

    /* Register STA events */
    ret = whd_wifi_set_event_handler(wifi_sta.whd_itf, sta_link_events,
                                     (whd_event_handler_t)sta_event_handler,
                                     &wifi_sta, &wifi_sta.event_handler_index);
    if (ret != WHD_SUCCESS)
    {
        LOG_W("Failed to register STA event handler: %lu", ret);
    }

    /* Register AP events (only if AP interface exists) */
    if (wifi_ap.whd_itf != NULL)
    {
        ret = whd_wifi_set_event_handler(wifi_ap.whd_itf, ap_link_events,
                                         (whd_event_handler_t)ap_event_handler,
                                         &wifi_ap, &wifi_ap.event_handler_index);
        if (ret != WHD_SUCCESS)
        {
            LOG_W("Failed to register AP event handler: %lu", ret);
        }
    }

    return CY_RSLT_SUCCESS;
}

/**
 * @brief WiFi initialization thread
 */
static void wifi_init_thread(void *parameter)
{
    static struct rt_wlan_device wlan_sta_dev, wlan_ap_dev;
    whd_result_t ret;
    rt_bool_t probe_sem_created = RT_FALSE;

    whd_gpio_t host_wake_pin = (whd_gpio_t)CYHAL_NC_PIN_VALUE;
#if defined(CYBSP_USING_PIN_NAME) && defined(CYBSP_HOST_WAKE_IRQ_PIN_NAME)
    /* Kconfig may define CYBSP_HOST_WAKE_IRQ_PIN_NAME as "" (meaning not configured). */
    if (CYBSP_HOST_WAKE_IRQ_PIN_NAME[0] != '\0')
    {
        rt_base_t pin = rt_pin_get(CYBSP_HOST_WAKE_IRQ_PIN_NAME);
        if (pin >= 0)
        {
            host_wake_pin = (whd_gpio_t)pin;
        }
    }
#elif defined(CYBSP_HOST_WAKE_IRQ_PIN)
    host_wake_pin = CYBSP_HOST_WAKE_IRQ_PIN;
#endif

    whd_oob_config_t oob_config = {
        .host_oob_pin      = host_wake_pin,  /* NC => use SDIO in-band interrupt */
        .dev_gpio_sel      = 0,
        .is_falling_edge   = (CYBSP_HOST_WAKE_IRQ_EVENT == CYHAL_GPIO_IRQ_FALL) ? WHD_TRUE : WHD_FALSE,
        .intr_priority     = 0, /* Unused in RT-Thread, rt_pin_attach_irq() doesn't support priority */
        .drive_mode        = CYHAL_GPIO_DRIVE_PULLUP, /* Internal pull-up keeps pin HIGH when unconnected */
        .init_drive_state  = WHD_TRUE  /* Initial state HIGH (idle) */
    };

    whd_sdio_config_t sdio_config = {
        .sdio_1bit_mode        = WHD_FALSE,
        .high_speed_sdio_clock = WHD_TRUE,
        .oob_config            = oob_config
    };

    if (wifi_mutex == RT_NULL)
    {
        wifi_mutex = rt_mutex_create("wifi_mtx", RT_IPC_FLAG_PRIO);
        if (wifi_mutex == RT_NULL)
        {
            LOG_E("Failed to create WiFi mutex");
            goto init_fail;
        }
        mutex_initialized = RT_TRUE;
    }
    else
    {
        mutex_initialized = RT_TRUE;
    }

    /* Set up WLAN device pointers */
    wifi_sta.wlan = &wlan_sta_dev;
    wifi_ap.wlan = &wlan_ap_dev;

    /* Create SDIO probe semaphore */
    cyhal_sdio.probe = rt_sem_create("sdio_probe", 0, RT_IPC_FLAG_PRIO);
    if (cyhal_sdio.probe == RT_NULL)
    {
        LOG_E("Failed to create SDIO probe semaphore");
        goto init_fail;
    }
    probe_sem_created = RT_TRUE;

    /* Initialize SDIO */
    if (cyhal_sdio_init(&cyhal_sdio) != CYHAL_SDIO_RET_NO_ERRORS)
    {
        LOG_E("SDIO initialization failed");
        goto init_fail;
    }

    rt_sem_take(cyhal_sdio.probe, RT_WAITING_FOREVER);
    rt_sem_delete(cyhal_sdio.probe);
    cyhal_sdio.probe = RT_NULL;
    probe_sem_created = RT_FALSE;

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
    /* The combined firmware is read from a file system. Wait until the board
     * has mounted it, otherwise the firmware file cannot be opened. */
    LOG_I("Waiting for the file system to be mounted...");
    whd_wait_fs_mount();
    LOG_I("File system is mounted");
#endif

    /* Print version info */
    rt_kprintf("\n");
    rt_kprintf("==================================================\n");
    rt_kprintf("  Aerolynq WiFi and Bluetooth Host Driver for RT-Thread\n");
    rt_kprintf("  For technical support contact xlx@xlxwlan.com\n");
    rt_kprintf("  www.aerolynq.com\n");
    rt_kprintf("==================================================\n");


    ret = whd_init(&whd_driver, &whd_config, &resource_ops, &whd_buffer_ops, &netif_if_ops);
    if (ret != WHD_SUCCESS)
    {
        LOG_E("WHD init failed: %lu", ret);
        goto init_fail;
    }

    /* Attach SDIO bus */
    ret = whd_bus_sdio_attach(whd_driver, &sdio_config, &cyhal_sdio);
    if (ret != WHD_SUCCESS)
    {
        LOG_E("SDIO attach failed: %lu", ret);
        goto init_fail;
    }

    /* Power on WiFi and create primary interface (STA) */
    ret = whd_wifi_on(whd_driver, &wifi_sta.whd_itf);
    if (ret != WHD_SUCCESS)
    {
        LOG_E("WiFi power on failed: %lu (0x%lx)", ret, ret);
        goto init_fail;
    }
    wifi_sta.is_initialized = RT_TRUE;

    /* Disable power save (WLC_SET_PM=0, i.e. PM0/Active) and
     * disable MPC (Minimum Power Consumption) so the radio stays
     * fully clocked - this gives the most stable iperf throughput. */
    {
        whd_result_t pm_ret, mpc_ret;

        pm_ret = whd_wifi_disable_powersave(wifi_sta.whd_itf);
        LOG_I("STA whd_wifi_disable_powersave: 0x%08lX", (unsigned long)pm_ret);

        mpc_ret = whd_wifi_set_iovar_value(wifi_sta.whd_itf, "mpc", 0);
        LOG_I("STA whd_wifi_set_iovar_value(mpc,0): 0x%08lX", (unsigned long)mpc_ret);
    }

#ifdef WHD_USING_ROAM
    /* Enable roaming in firmware by default */
    {
        whd_result_t roam_off_ret;

        roam_off_ret = whd_wifi_set_iovar_value(wifi_sta.whd_itf, IOVAR_STR_ROAM_OFF, 0);

        if (roam_off_ret == WHD_SUCCESS)
        {
            LOG_I("Roaming enabled (roam_off=0)");
        }
        else
        {
            LOG_W("Set roam_off failed: %lu", roam_off_ret);
            roaming_enabled = RT_FALSE;
        }
    }
#endif /* WHD_USING_ROAM */

    /* Create secondary interface (AP) */
    ret = whd_add_secondary_interface(whd_driver, NULL, &wifi_ap.whd_itf);
    if (ret != WHD_SUCCESS)
    {
        LOG_E("Failed to create AP interface: %lu", ret);
        /* Continue without AP support */
    }
    else
    {
        wifi_ap.is_initialized = RT_TRUE;
        /* Power-save / MPC settings for the AP are applied later,
         * in the AP-start path, after whd_wifi_start_ap() succeeds. */
    }

    /* Register event handlers */
    register_event_handlers();

    /* Register WLAN devices with RT-Thread */
    if (rt_wlan_dev_register(&wlan_sta_dev, RT_WLAN_DEVICE_STA_NAME,
                             &wlan_ops, 0, &wifi_sta) != RT_EOK)
    {
        LOG_E("Failed to register STA device");
        goto init_fail;
    }

    if (wifi_ap.is_initialized)
    {
        if (rt_wlan_dev_register(&wlan_ap_dev, RT_WLAN_DEVICE_AP_NAME,
                                 &wlan_ops, 0, &wifi_ap) != RT_EOK)
        {
            LOG_E("Failed to register AP device");
            /* Continue with STA only */
        }
    }

    /* Set operating modes */
    if (rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION) != RT_EOK)
    {
        LOG_E("Failed to set STA mode");
    }

   if (wifi_ap.is_initialized)
    {
        if (rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP) != RT_EOK)
        {
            LOG_E("Failed to set AP mode");
        }
    }

    LOG_I("WiFi initialization complete");

    /***************************************************************************
     * Bluetooth Initialization
     *
     * In RT-Thread, stm32_cypal_bt_init() is not needed because
     *       cyhal_uart.c uses the RT-Thread Serial device abstraction.
     *       The UART device (e.g., "uart1") must be configured in the board
     *       BSP with hardware flow control (CTS/RTS) for BT HCI.
     ***************************************************************************/
#ifdef WHD_USING_BLUETOOTH
    {
        LOG_I("Initializing Bluetooth...");

        /* Initialize BLE platform configuration
         * This resolves Kconfig pin names (if WHD_BT_USING_PIN_NAME is enabled)
         * to actual pin numbers at runtime. Must be called before
         * cybt_platform_config_init(). */
        if (ble_platform_cfg_init() != 0)
        {
            LOG_E("BLE platform configuration failed!");
            /* Continue anyway - some pins might still work */
        }

        /* Configure BLE Platform. This sets up HCI UART parameters from
         * ble_app_cfg.c. */
        cybt_platform_config_init(&ble_platform_cfg_settings);

        /* Start BLE application - this calls wiced_bt_stack_init() internally */
        ble_app_init();

        LOG_I("Bluetooth initialization complete");
    }
#endif

    return;

init_fail:
    if (probe_sem_created && cyhal_sdio.probe != RT_NULL)
    {
        rt_sem_delete(cyhal_sdio.probe);
        cyhal_sdio.probe = RT_NULL;
    }

    /* Best-effort clean up RT-Thread objects allocated in this function. */
    if (wifi_mutex != RT_NULL)
    {
        rt_mutex_delete(wifi_mutex);
        wifi_mutex = RT_NULL;
    }
    mutex_initialized = RT_FALSE;
}

/**
 * @brief Hardware WiFi initialization entry point
 */
static int rt_hw_wifi_init(void)
{
    rt_kprintf("\n[WiFi] rt_hw_wifi_init called!\n");

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
    /* The firmware is read from a file system that the board mounts during
     * the component initialization. The mount happens at a later init level
     * than this function, so the WiFi initialization must not run here: it
     * would block the init sequence and the file system would never be
     * mounted.
     *
     * The initialization therefore runs in its own thread. The thread has a
     * low priority, so the init sequence of the main thread finishes first
     * and the file system is mounted before the thread continues. The thread
     * then waits for the mount with whd_wait_fs_mount(). */
    {
        rt_thread_t tid;

        tid = rt_thread_create("whd_init", wifi_init_thread, RT_NULL,
                               CY_WIFI_INIT_THREAD_STACK_SIZE,
                               CY_WIFI_INIT_THREAD_PRIORITY, 20);
        if (tid != RT_NULL)
        {
            rt_thread_startup(tid);
            LOG_I("WiFi init moved to the 'whd_init' thread (file system mode)");
        }
        else
        {
            LOG_E("Failed to create the 'whd_init' thread");
            return -RT_ENOMEM;
        }
    }
#else
    wifi_init_thread(RT_NULL);
#endif

    return RT_EOK;
}
INIT_ENV_EXPORT(rt_hw_wifi_init);

/*******************************************************************************
 * Public API for Advanced Control
 ******************************************************************************/

/**
 * @brief Get WHD driver instance
 */
whd_driver_t whd_wlan_get_driver(void)
{
    return whd_driver;
}

/**
 * @brief Get STA interface
 */
whd_interface_t whd_wlan_get_sta_interface(void)
{
    return wifi_sta.whd_itf;
}

/**
 * @brief Get AP interface
 */
whd_interface_t whd_wlan_get_ap_interface(void)
{
    return wifi_ap.whd_itf;
}

/**
 * @brief Check if STA is connected (queries firmware for real status)
 * @return RT_TRUE if connected, RT_FALSE otherwise
 */
rt_bool_t whd_wlan_is_sta_connected(void)
{
    /* First check local state */
    if (!sta_link_up)
        return RT_FALSE;

    /* Verify with firmware - whd_wifi_is_ready_to_transceive checks actual link status */
    if (wifi_sta.whd_itf != RT_NULL)
    {
        if (whd_wifi_is_ready_to_transceive(wifi_sta.whd_itf) != WHD_SUCCESS)
        {
            /* Firmware says not connected, sync local state */
            LOG_W("Firmware link down, syncing state");
            sta_link_up = RT_FALSE;
            wifi_sta.state = WIFI_STATE_IDLE;
            return RT_FALSE;
        }
    }

    return RT_TRUE;
}

/**
 * @brief Check if AP is running
 */
rt_bool_t whd_wlan_is_ap_running(void)
{
    return ap_link_up;
}

/**
 * @brief Enable auto-reconnect
 */
void whd_wlan_enable_auto_reconnect(rt_bool_t enable)
{
    if (!enable)
    {
        if (wifi_lock() == RT_EOK)
        {
            connected_ap.is_valid = RT_FALSE;
            wifi_unlock();
        }
        else
        {
            connected_ap.is_valid = RT_FALSE;
        }
    }
}

#ifdef WHD_USING_ROAM
/*******************************************************************************
 * Roaming Helper Functions
 ******************************************************************************/

/**
 * @brief Check if string is a valid band name
 */
static rt_bool_t is_band_str(const char *str)
{
    return (rt_strcmp(str, "2g") == 0 || rt_strcmp(str, "2G") == 0 ||
            rt_strcmp(str, "5g") == 0 || rt_strcmp(str, "5G") == 0 ||
            rt_strcmp(str, "all") == 0 || rt_strcmp(str, "ALL") == 0 ||
            rt_strcmp(str, "auto") == 0 || rt_strcmp(str, "AUTO") == 0);
}

/**
 * @brief Parse band string to value
 */
static uint32_t parse_band(const char *str)
{
    if (rt_strcmp(str, "2g") == 0 || rt_strcmp(str, "2G") == 0)
        return WLC_BAND_2G;
    else if (rt_strcmp(str, "5g") == 0 || rt_strcmp(str, "5G") == 0)
        return WLC_BAND_5G;
    else if (rt_strcmp(str, "all") == 0 || rt_strcmp(str, "ALL") == 0)
        return WLC_BAND_ALL;
    else if (rt_strcmp(str, "auto") == 0 || rt_strcmp(str, "AUTO") == 0)
        return WLC_BAND_AUTO;
    return WLC_BAND_2G;  /* Default for CYW43012 */
}

static const char* band_to_str(uint32_t band)
{
    switch (band)
    {
        case WLC_BAND_2G:   return "2G";
        case WLC_BAND_5G:   return "5G";
        case WLC_BAND_ALL:  return "ALL";
        case WLC_BAND_AUTO: return "AUTO";
        default:            return "?";
    }
}

static rt_bool_t parse_int32_strict(const char *str, int32_t *out_value)
{
    char *endptr;
    long value;

    if (str == RT_NULL || out_value == RT_NULL || *str == '\0')
        return RT_FALSE;

    value = strtol(str, &endptr, 0);
    if (endptr == str || *endptr != '\0')
        return RT_FALSE;

    if (value < INT32_MIN || value > INT32_MAX)
        return RT_FALSE;

    *out_value = (int32_t)value;
    return RT_TRUE;
}

static rt_bool_t parse_uint32_strict(const char *str, uint32_t *out_value)
{
    char *endptr;
    unsigned long value;

    if (str == RT_NULL || out_value == RT_NULL || *str == '\0')
        return RT_FALSE;

    /* Reject negative values explicitly */
    if (str[0] == '-')
        return RT_FALSE;

    value = strtoul(str, &endptr, 0);
    if (endptr == str || *endptr != '\0')
        return RT_FALSE;

    if (value > 0xFFFFFFFFUL)
        return RT_FALSE;

    *out_value = (uint32_t)value;
    return RT_TRUE;
}

/**
 * @brief Get roam time threshold (wrapper for WHD API)
 */
rt_err_t whd_wlan_get_roam_time_threshold(uint32_t *threshold)
{
    whd_result_t ret;

    if (wifi_sta.whd_itf == NULL || threshold == NULL)
        return -RT_ERROR;

    ret = whd_wifi_get_roam_time_threshold(wifi_sta.whd_itf, threshold);
    if (ret == WHD_SUCCESS)
        return RT_EOK;

    return -RT_ERROR;
}

/**
 * @brief Set roam time threshold (wrapper for WHD API)
 */
rt_err_t whd_wlan_set_roam_time_threshold(uint32_t *threshold)
{
    whd_result_t ret;

    if (wifi_sta.whd_itf == NULL || threshold == NULL || *threshold == 0)
        return -RT_ERROR;

    ret = whd_wifi_set_roam_time_threshold(wifi_sta.whd_itf, *threshold);
    if (ret == WHD_SUCCESS)
        return RT_EOK;

    return -RT_ERROR;
}

/**
 * @brief MSH command to get/set roaming parameters
 * Usage:
 *   wifi_roam                         - show all roaming parameters
 *   wifi_roam trigger [2g|5g|all] [-70] - get/set roam trigger (dBm) (band required for set)
 *   wifi_roam delta [2g|5g|all] [10]  - get/set roam delta (dB) (band required for set)
 *   wifi_roam period [10]             - get/set roam scan period (seconds)
 *   wifi_roam time [3000]             - get/set roam time threshold (ms)
 *   wifi_roam off [0|1]               - get/set roam off (0=enabled, 1=disabled)
 */

static int cmd_wifi_roam(int argc, char **argv)
{
    if (wifi_sta.whd_itf == NULL)
    {
        rt_kprintf("WiFi STA interface not initialized\n");
        return -1;
    }


    if (argc == 1)
    {
        int32_t roam_trigger_2g = 0, roam_trigger_5g = 0;
        uint32_t roam_delta_2g = 0, roam_delta_5g = 0;
        uint32_t roam_period = 0;
        uint32_t roam_time = 0;
        uint32_t roam_off = 0;

        rt_kprintf("=== Roaming Parameters ===\n");

        if (whd_wifi_get_iovar_value(wifi_sta.whd_itf, IOVAR_STR_ROAM_OFF, &roam_off) == WHD_SUCCESS)
            rt_kprintf("  roam_off:          %lu (%s)\n", roam_off, roam_off ? "disabled" : "enabled");
        else
            rt_kprintf("  roam_off:          (read failed)\n");


        if (whd_wifi_get_roam_trigger(wifi_sta.whd_itf, &roam_trigger_2g, WLC_BAND_2G) == WHD_SUCCESS)
            rt_kprintf("  roam_trigger(2G):  %ld dBm\n", roam_trigger_2g);
        else
            rt_kprintf("  roam_trigger(2G):  (read failed)\n");

        if (whd_wifi_get_roam_trigger(wifi_sta.whd_itf, &roam_trigger_5g, WLC_BAND_5G) == WHD_SUCCESS)
            rt_kprintf("  roam_trigger(5G):  %ld dBm\n", roam_trigger_5g);
        else
            rt_kprintf("  roam_trigger(5G):  (N/A - single band chip)\n");


        if (whd_wifi_get_roam_delta(wifi_sta.whd_itf, &roam_delta_2g, WLC_BAND_2G) == WHD_SUCCESS)
            rt_kprintf("  roam_delta(2G):    %lu dB\n", roam_delta_2g);
        else
            rt_kprintf("  roam_delta(2G):    (read failed)\n");

        if (whd_wifi_get_roam_delta(wifi_sta.whd_itf, &roam_delta_5g, WLC_BAND_5G) == WHD_SUCCESS)
            rt_kprintf("  roam_delta(5G):    %lu dB\n", roam_delta_5g);
        else
            rt_kprintf("  roam_delta(5G):    (N/A - single band chip)\n");

        if (whd_wifi_get_roam_scan_period(wifi_sta.whd_itf, &roam_period) == WHD_SUCCESS)
            rt_kprintf("  roam_period:       %lu seconds\n", roam_period);
        else
            rt_kprintf("  roam_period:       (read failed)\n");

        if (whd_wifi_get_roam_time_threshold(wifi_sta.whd_itf, &roam_time) == WHD_SUCCESS)
            rt_kprintf("  roam_time:         %lu ms\n", roam_time);
        else
            rt_kprintf("  roam_time:         (read failed)\n");

        return 0;
    }


    if (argc >= 2)
    {
        const char *param = argv[1];

        if (rt_strcmp(param, "trigger") == 0)
        {
            if (argc == 2)
            {

                int32_t value = 0;
                if (whd_wifi_get_roam_trigger(wifi_sta.whd_itf, &value, WLC_BAND_2G) == WHD_SUCCESS)
                    rt_kprintf("roam_trigger(2G): %ld dBm\n", value);
                else
                    rt_kprintf("Failed to get roam_trigger\n");
                return 0;
            }

            if (argc == 3)
            {
                if (!is_band_str(argv[2]))
                {
                    rt_kprintf("Error: invalid band. Use: wifi_roam trigger [2g|5g|all|auto]\n");
                    return -1;
                }

                uint32_t band = parse_band(argv[2]);
                int32_t value = 0;
                if (whd_wifi_get_roam_trigger(wifi_sta.whd_itf, &value, band) == WHD_SUCCESS)
                    rt_kprintf("roam_trigger(%s): %ld dBm\n", band_to_str(band), value);
                else
                    rt_kprintf("Failed to get roam_trigger(%s)\n", band_to_str(band));
                return 0;
            }

            if (argc == 4)
            {
                const char *band_arg = RT_NULL;
                const char *value_arg = RT_NULL;
                uint32_t band;
                int32_t value;

                if (is_band_str(argv[2]))
                {
                    band_arg = argv[2];
                    value_arg = argv[3];
                }
                else if (is_band_str(argv[3]))
                {
                    value_arg = argv[2];
                    band_arg = argv[3];
                }
                else
                {
                    rt_kprintf("Error: band required for set. Use: wifi_roam trigger [2g|5g|all|auto] -70\n");
                    return -1;
                }

                band = parse_band(band_arg);

                if (!parse_int32_strict(value_arg, &value))
                {
                    rt_kprintf("Error: invalid roam_trigger value: %s\n", value_arg);
                    return -1;
                }

                if (value >= 0)
                {
                    rt_kprintf("Error: roam_trigger must be negative (e.g., -70)\n");
                    return -1;
                }

                if (whd_wifi_set_roam_trigger(wifi_sta.whd_itf, value, band) == WHD_SUCCESS)
                    rt_kprintf("Set roam_trigger(%s) to %ld dBm\n", band_to_str(band), value);
                else
                    rt_kprintf("Failed to set roam_trigger(%s)\n", band_to_str(band));
                return 0;
            }

            rt_kprintf("Usage: wifi_roam trigger [2g|5g|all|auto] [-70]\n");
            return -1;
        }
        else if (rt_strcmp(param, "delta") == 0)
        {
            if (argc == 2)
            {
                uint32_t value = 0;
                if (whd_wifi_get_roam_delta(wifi_sta.whd_itf, &value, WLC_BAND_2G) == WHD_SUCCESS)
                    rt_kprintf("roam_delta(2G): %lu dB\n", value);
                else
                    rt_kprintf("Failed to get roam_delta\n");
                return 0;
            }

            if (argc == 3)
            {

                if (!is_band_str(argv[2]))
                {
                    rt_kprintf("Error: invalid band. Use: wifi_roam delta [2g|5g|all|auto]\n");
                    return -1;
                }

                uint32_t band = parse_band(argv[2]);
                uint32_t value = 0;
                if (whd_wifi_get_roam_delta(wifi_sta.whd_itf, &value, band) == WHD_SUCCESS)
                    rt_kprintf("roam_delta(%s): %lu dB\n", band_to_str(band), value);
                else
                    rt_kprintf("Failed to get roam_delta(%s)\n", band_to_str(band));
                return 0;
            }

            if (argc == 4)
            {

                const char *band_arg = RT_NULL;
                const char *value_arg = RT_NULL;
                uint32_t band;
                uint32_t value;

                if (is_band_str(argv[2]))
                {
                    band_arg = argv[2];
                    value_arg = argv[3];
                }
                else if (is_band_str(argv[3]))
                {
                    value_arg = argv[2];
                    band_arg = argv[3];
                }
                else
                {
                    rt_kprintf("Error: band required for set. Use: wifi_roam delta [2g|5g|all|auto] 10\n");
                    return -1;
                }

                band = parse_band(band_arg);

                if (!parse_uint32_strict(value_arg, &value))
                {
                    rt_kprintf("Error: invalid roam_delta value: %s\n", value_arg);
                    return -1;
                }

                if (whd_wifi_set_roam_delta(wifi_sta.whd_itf, value, band) == WHD_SUCCESS)
                    rt_kprintf("Set roam_delta(%s) to %lu dB\n", band_to_str(band), value);
                else
                    rt_kprintf("Failed to set roam_delta(%s)\n", band_to_str(band));
                return 0;
            }

            rt_kprintf("Usage: wifi_roam delta [2g|5g|all|auto] [10]\n");
            return -1;
        }
        else if (rt_strcmp(param, "period") == 0)
        {
            if (argc == 3)
            {
                uint32_t value;
                if (!parse_uint32_strict(argv[2], &value))
                {
                    rt_kprintf("Error: invalid roam_period value: %s\n", argv[2]);
                    return -1;
                }
                if (whd_wifi_set_roam_scan_period(wifi_sta.whd_itf, value) == WHD_SUCCESS)
                    rt_kprintf("Set roam_period to %lu seconds\n", value);
                else
                    rt_kprintf("Failed to set roam_period\n");
            }
            else
            {
                uint32_t value = 0;
                if (whd_wifi_get_roam_scan_period(wifi_sta.whd_itf, &value) == WHD_SUCCESS)
                    rt_kprintf("roam_period: %lu seconds\n", value);
                else
                    rt_kprintf("Failed to get roam_period\n");
            }
        }
        else if (rt_strcmp(param, "time") == 0)
        {
            if (argc == 3)
            {
                uint32_t value;
                if (!parse_uint32_strict(argv[2], &value))
                {
                    rt_kprintf("Error: invalid roam_time value: %s\n", argv[2]);
                    return -1;
                }
                if (whd_wifi_set_roam_time_threshold(wifi_sta.whd_itf, value) == WHD_SUCCESS)
                    rt_kprintf("Set roam_time to %lu ms\n", value);
                else
                    rt_kprintf("Failed to set roam_time\n");
            }
            else
            {
                uint32_t value = 0;
                if (whd_wifi_get_roam_time_threshold(wifi_sta.whd_itf, &value) == WHD_SUCCESS)
                    rt_kprintf("roam_time: %lu ms\n", value);
                else
                    rt_kprintf("Failed to get roam_time\n");
            }
        }
        else if (rt_strcmp(param, "off") == 0)
        {
            if (argc == 3)
            {
                uint32_t value;
                if (!parse_uint32_strict(argv[2], &value) || value > 1)
                {
                    rt_kprintf("Error: roam_off must be 0 or 1\n");
                    return -1;
                }
                if (whd_wifi_set_iovar_value(wifi_sta.whd_itf, IOVAR_STR_ROAM_OFF, value) == WHD_SUCCESS)
                    rt_kprintf("Set roam_off to %lu (%s)\n", value, value ? "disabled" : "enabled");
                else
                    rt_kprintf("Failed to set roam_off\n");
            }
            else
            {
                uint32_t value = 0;
                if (whd_wifi_get_iovar_value(wifi_sta.whd_itf, IOVAR_STR_ROAM_OFF, &value) == WHD_SUCCESS)
                    rt_kprintf("roam_off: %lu (%s)\n", value, value ? "disabled" : "enabled");
                else
                    rt_kprintf("Failed to get roam_off\n");
            }
        }
        else
        {
            rt_kprintf("Usage: wifi_roam [param] [value] [band]\n");
            rt_kprintf("  wifi_roam                      - show all parameters\n");
            rt_kprintf("  wifi_roam trigger [2g|5g|all|auto] [-70] - roam trigger (dBm) (band required for set)\n");
            rt_kprintf("  wifi_roam delta [2g|5g|all|auto] [10]    - roam delta (dB) (band required for set)\n");
            rt_kprintf("  wifi_roam period [10]          - scan period (seconds)\n");
            rt_kprintf("  wifi_roam time [3000]          - time threshold (ms)\n");
            rt_kprintf("  wifi_roam off [0|1]            - 0=enable, 1=disable roaming\n");
            rt_kprintf("\nBand: 2g, 5g, all, auto (default: 2g for CYW43012)\n");
            return -1;
        }
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_roam, wifi_roam, Get or set WiFi roaming parameters);
#endif /* WHD_USING_ROAM */

#ifdef WHD_USING_MFG_TEST
/*******************************************************************************
 * WiFi MFG Test Mode Support
 ******************************************************************************/

#include "mfg_test_common_api.h"

/* Export mfg_test_mode so mfg_test_common.c can set it to exit the loop */
rt_bool_t mfg_test_mode = RT_FALSE;

static int cmd_rf_mode(int argc, char *argv[])
{
    whd_result_t res;

    if (mfg_test_mode)
    {
        rt_kprintf("Already in MFG test mode\n");
        return 0;
    }

    /* Check if WiFi is initialized */
    if (whd_driver == NULL)
    {
        rt_kprintf("Error: WiFi driver not initialized\n");
        return -1;
    }

    rt_kprintf("Entering MFG RF test mode...\n");

    /* Deinit current WiFi connection if any */
    if (wifi_sta.whd_itf != NULL)
    {
        rt_kprintf("Disconnecting WiFi...\n");
        whd_wifi_leave(wifi_sta.whd_itf);
        rt_thread_mdelay(500);
    }

    /* MFG firmware is already loaded at boot when WHD_USING_MFG_TEST is enabled.
     * The SConscript build system automatically selects MFG firmware instead of
     * normal firmware during compilation. No runtime firmware switching is needed. */
    rt_kprintf("Using MFG test firmware (loaded at boot)\n");

    /* Set interface name to match firmware interface name */
    //wl_set_interface_name("wl0");

    /* Set interface handle for mfg test library */
    if (wifi_sta.whd_itf != NULL)
    {
        wl_set_sta_interface_handle(wifi_sta.whd_itf);
    }

    /* Enter MFG test mode - run in current thread */
    mfg_test_mode = RT_TRUE;

    rt_kprintf("\n========================================\n");
    rt_kprintf("  MFG Test Mode Activated\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Ready to receive wl.exe commands\n");
    rt_kprintf("PC usage: wl.exe --serial COM_PORT ver\n");
    rt_kprintf("To exit: Press Enter after closing wl.exe\n");
    rt_kprintf("========================================\n\n");
    rt_kprintf("Waiting for wl.exe commands...\n\n");

    /* Run MFG command handler loop in current thread */
    while (mfg_test_mode)
    {
        /* wl_remote_command_handler() reads serial data via rt_device_read() */
        int result = wl_remote_command_handler(NULL);

        if (result == -2)
        {
            /* User pressed Enter to exit */
            break;
        }

        /* Check if user requested exit */
        if (!mfg_test_mode)
        {
            break;
        }
    }

    rt_kprintf("\n========================================\n");
    rt_kprintf("  MFG Test Mode Exited\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Returning to normal msh mode...\n");
    rt_kprintf("You can now use msh commands.\n");
    rt_kprintf("========================================\n\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_rf_mode, rf_mode, Enter WiFi MFG RF test mode);

static int cmd_normalmode(int argc, char *argv[])
{
    if (!mfg_test_mode)
    {
        rt_kprintf("Not in MFG test mode\n");
        return 0;
    }

    rt_kprintf("Exiting MFG test mode...\n");

    /* Stop MFG test loop */
    mfg_test_mode = RT_FALSE;

    rt_kprintf("MFG test mode exited.\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_normalmode, normalmode, Exit MFG test mode and return to normal);

#endif /* WHD_USING_MFG_TEST */

/*******************************************************************************
 * Accessor functions for whd_low_power.c (WiFi Low-Power Offload module)
 *
 * These functions expose the STA context to the low-power module without
 * making internal structures globally visible.
 ******************************************************************************/
whd_interface_t whd_lp_get_sta_itf(void)
{
    return wifi_sta.whd_itf;
}

struct rt_wlan_device *whd_lp_get_sta_wlan(void)
{
    return wifi_sta.wlan;
}

rt_err_t whd_lp_set_powersave(struct rt_wlan_device *wlan, int level)
{
    return drv_wlan_set_powersave(wlan, level);
}

/* BLE MSH commands are now implemented in bt_application/ble_msh_cmd.c */
