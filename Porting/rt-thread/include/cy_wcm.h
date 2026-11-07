/***********************************************************************************************//**
 * \file cy_wcm.h
 *
 * \brief
 * Lightweight WiFi Connection Manager (WCM) shim for RT-Thread porting layer.
 *
 * This header provides ONLY the types and function declarations required by the
 * WPA3 External Supplicant (External/wpa3-external-supplicant).  It replaces the
 * full Infineon wifi-connection-manager cy_wcm.h so that the WPA3 component can
 * compile without pulling in the entire WCM library.
 *
 * The actual function implementations are provided in aerolynq_bt_wlan.c via
 * lightweight bridge functions that map WCM calls to the RT-Thread WLAN driver
 * internals.
 *
 * Note: The WPA3 supplicant source code (Infineon original) does
 * \c \#include \c "cy_wcm.h" and must not be modified, so this file keeps
 * the original name.
 *
 * \copyright
 * Copyright 2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#ifndef CY_WCM_H
#define CY_WCM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cy_result.h"
#include "whd_types.h"
#include "whd.h"

/*******************************************************************************
 * Macros
 ******************************************************************************/
#ifndef CY_WCM_MAC_ADDR_LEN
#define CY_WCM_MAC_ADDR_LEN    6
#endif

/** Generic middleware-level error for WCM shim functions */
#ifndef CY_RSLT_MW_ERROR
#define CY_RSLT_MW_ERROR        ((cy_rslt_t)1U)
#endif

/*******************************************************************************
 * Enumerations
 ******************************************************************************/

/**
 * WCM interface type \u2014 only the values used by WPA3 supplicant are defined.
 */
typedef enum
{
    CY_WCM_INTERFACE_TYPE_STA = 0,   /**< STA / client interface  */
    CY_WCM_INTERFACE_TYPE_AP,        /**< SoftAP interface        */
    CY_WCM_INTERFACE_TYPE_AP_STA     /**< Concurrent AP + STA     */
} cy_wcm_interface_t;

/**
 * WCM event type \u2014 the WPA3 supplicant registers a callback that receives these.
 */
typedef enum
{
    CY_WCM_EVENT_CONNECTING = 0,       /**< STA connecting to an AP         */
    CY_WCM_EVENT_CONNECTED,            /**< STA connected to the AP         */
    CY_WCM_EVENT_CONNECT_FAILED,       /**< STA connection to the AP failed */
    CY_WCM_EVENT_RECONNECTED,          /**< STA reconnected to the AP       */
    CY_WCM_EVENT_DISCONNECTED,         /**< STA disconnected from the AP    */
    CY_WCM_EVENT_IP_CHANGED,           /**< IP address changed              */
    CY_WCM_EVENT_INITIATED_RETRY,      /**< WCM will retry connection       */
    CY_WCM_EVENT_STA_JOINED_SOFTAP,    /**< STA joined SoftAP               */
    CY_WCM_EVENT_STA_LEFT_SOFTAP       /**< STA left SoftAP                 */
} cy_wcm_event_t;

/**
 * IP address version
 */
typedef enum
{
    CY_WCM_IP_VER_V4 = 4,
    CY_WCM_IP_VER_V6 = 6
} cy_wcm_ip_version_t;

/*******************************************************************************
 * Data Types
 ******************************************************************************/

typedef struct
{
    cy_wcm_ip_version_t version;
    union
    {
        uint32_t v4;
        uint32_t v6[4];
    } ip;
} cy_wcm_ip_address_t;

/** Reason code for disconnect events (simplified) */
typedef uint32_t cy_wcm_reason_code;

/** MAC address type */
typedef uint8_t cy_wcm_mac_t[CY_WCM_MAC_ADDR_LEN];

/**
 * Event data union passed to event callbacks.
 */
typedef union
{
    cy_wcm_ip_address_t ip_addr;   /**< IP for CY_WCM_EVENT_IP_CHANGED           */
    cy_wcm_mac_t        sta_mac;   /**< MAC for STA_JOINED / STA_LEFT events      */
    cy_wcm_reason_code  reason;    /**< Reason code for DISCONNECTED events        */
} cy_wcm_event_data_t;

/**
 * Event callback function pointer type.
 */
typedef void (*cy_wcm_event_callback_t)(cy_wcm_event_t event, cy_wcm_event_data_t *event_data);

/*******************************************************************************
 * Function Declarations (implemented in aerolynq_bt_wlan.c)
 ******************************************************************************/

/**
 * @brief Get WHD interface handle for a given WCM interface type.
 *
 * Bridge function: maps CY_WCM_INTERFACE_TYPE_STA to the driver's STA WHD
 * interface and CY_WCM_INTERFACE_TYPE_AP to the AP WHD interface.
 *
 * @param[in]  interface_type  WCM interface type
 * @param[out] whd_iface       Pointer to receive WHD interface handle
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cy_wcm_get_whd_interface(cy_wcm_interface_t interface_type, whd_interface_t *whd_iface);

/**
 * @brief Register a WCM event callback.
 *
 * The WPA3 supplicant uses this to receive connection state notifications.
 * The driver fires these events from link_up_handler / link_down_handler /
 * wifi_connect_internal to drive WPA3 workspace cleanup on disconnect or
 * re-connection.
 *
 * @param[in] event_callback  Callback function pointer
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cy_wcm_register_event_callback(cy_wcm_event_callback_t event_callback);

/**
 * @brief Deregister a previously registered WCM event callback.
 *
 * @param[in] event_callback  Callback function pointer to deregister
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cy_wcm_deregister_event_callback(cy_wcm_event_callback_t event_callback);

#ifdef __cplusplus
}
#endif

#endif /* CY_WCM_H */
