/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-24     Aerolynq     Initial version - BLE peripheral application
 */

#ifndef __BLE_APP_H__
#define __BLE_APP_H__

#include <rtthread.h>
#include "wiced_bt_dev.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_gatt.h"
#include "wiced_bt_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Constants
 ******************************************************************************/

/* Application timeout values */
#define BLE_APP_TIMEOUT_SECONDS             1
#define BLE_APP_CONN_IDLE_TIMEOUT_SECONDS   30

/* Standard 16-bit UUIDs used by this application */
#ifndef UUID_SERVICE_GAP
#define UUID_SERVICE_GAP                    0x1800
#endif
#ifndef UUID_SERVICE_GATT
#define UUID_SERVICE_GATT                   0x1801
#endif
#ifndef UUID_SERVICE_DEVICE_INFORMATION
#define UUID_SERVICE_DEVICE_INFORMATION     0x180A
#endif
#ifndef UUID_SERVICE_BATTERY
#define UUID_SERVICE_BATTERY                0x180F
#endif

#ifndef UUID_CHARACTERISTIC_DEVICE_NAME
#define UUID_CHARACTERISTIC_DEVICE_NAME     0x2A00
#endif
#ifndef UUID_CHARACTERISTIC_APPEARANCE
#define UUID_CHARACTERISTIC_APPEARANCE      0x2A01
#endif
#ifndef UUID_CHARACTERISTIC_MANUFACTURER_NAME_STRING
#define UUID_CHARACTERISTIC_MANUFACTURER_NAME_STRING 0x2A29
#endif
#ifndef UUID_CHARACTERISTIC_MODEL_NUMBER_STRING
#define UUID_CHARACTERISTIC_MODEL_NUMBER_STRING 0x2A24
#endif
#ifndef UUID_CHARACTERISTIC_FIRMWARE_REVISION_STRING
#define UUID_CHARACTERISTIC_FIRMWARE_REVISION_STRING 0x2A26
#endif
#ifndef UUID_CHARACTERISTIC_BATTERY_LEVEL
#define UUID_CHARACTERISTIC_BATTERY_LEVEL   0x2A19
#endif

/* GATT database handles */
enum ble_app_gatt_handles
{
    /* GAP Service */
    HANDLE_BLE_GAP_SERVICE = 0x0001,
    HANDLE_BLE_GAP_DEVICE_NAME,
    HANDLE_BLE_GAP_DEVICE_NAME_VAL,
    HANDLE_BLE_GAP_APPEARANCE,
    HANDLE_BLE_GAP_APPEARANCE_VAL,

    /* GATT Service */
    HANDLE_BLE_GATT_SERVICE = 0x0010,

    /* Custom Data Service - for data transfer */
    HANDLE_BLE_DATA_SERVICE = 0x0020,
    HANDLE_BLE_DATA_CHAR_TX,            /* TX: Device to Phone (Notify) */
    HANDLE_BLE_DATA_CHAR_TX_VAL,
    HANDLE_BLE_DATA_CHAR_TX_CCCD,       /* Client Characteristic Configuration */
    HANDLE_BLE_DATA_CHAR_RX,            /* RX: Phone to Device (Write) */
    HANDLE_BLE_DATA_CHAR_RX_VAL,

    /* Device Information Service */
    HANDLE_BLE_DEV_INFO_SERVICE = 0x0040,
    HANDLE_BLE_DEV_INFO_MFR_NAME,
    HANDLE_BLE_DEV_INFO_MFR_NAME_VAL,
    HANDLE_BLE_DEV_INFO_MODEL_NUM,
    HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL,
    HANDLE_BLE_DEV_INFO_FW_VER,
    HANDLE_BLE_DEV_INFO_FW_VER_VAL,

    /* Battery Service */
    HANDLE_BLE_BATTERY_SERVICE = 0x0060,
    HANDLE_BLE_BATTERY_LEVEL,
    HANDLE_BLE_BATTERY_LEVEL_VAL,
};

/* Custom Service UUID: 12345678-1234-5678-1234-56789abcdef0 */
#define UUID_BLE_DATA_SERVICE           0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, \
                                        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12

/* TX Characteristic UUID: 12345678-1234-5678-1234-56789abcdef1 */
#define UUID_BLE_DATA_CHAR_TX           0xf1, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, \
                                        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12

/* RX Characteristic UUID: 12345678-1234-5678-1234-56789abcdef2 */
#define UUID_BLE_DATA_CHAR_RX           0xf2, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, \
                                        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12

/* Maximum data length for TX/RX */
#define BLE_APP_MAX_DATA_LEN            128

/* Client Configuration flags */
#define GATT_CLIENT_CONFIG_NONE         0x0000
#define GATT_CLIENT_CONFIG_NOTIFICATION 0x0001
#define GATT_CLIENT_CONFIG_INDICATION   0x0002

/*******************************************************************************
 * Data Types
 ******************************************************************************/

/* BLE application state */
typedef struct
{
    wiced_bt_device_address_t   peer_addr;      /* Connected peer address */
    uint16_t                    conn_id;        /* Connection ID */
    uint16_t                    peer_mtu;       /* Peer MTU size */
    uint16_t                    cccd_flags;     /* Client config descriptor value */
    uint8_t                     is_connected;   /* Connection status */
    uint8_t                     battery_level;  /* Simulated battery level */
} ble_app_state_t;

/* RX data callback type */
typedef void (*ble_app_rx_callback_t)(const uint8_t *data, uint16_t len);

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize BT stack only (called on power-up)
 * @note This only initializes the Bluetooth stack, not GATT.
 *       Call ble_app_gatt_init() to initialize BLE GATT service.
 * @return RT_EOK on success
 */
rt_err_t ble_app_init(void);

/**
 * @brief Initialize BLE GATT service (called via 'ble gatt start' command)
 * @note This registers GATT callback and initializes GATT database.
 *       Must be called after ble_app_init() and BTM_ENABLED_EVT.
 * @return RT_EOK on success
 */
rt_err_t ble_app_gatt_init(void);

/**
 * @brief Check if BT stack is initialized
 * @return RT_TRUE if initialized
 */
rt_bool_t ble_app_is_initialized(void);

/**
 * @brief Check if GATT is initialized
 * @return RT_TRUE if initialized
 */
rt_bool_t ble_app_is_gatt_initialized(void);

/**
 * @brief Check if a client is connected
 * @return RT_TRUE if connected
 */
rt_bool_t ble_app_is_connected(void);

/**
 * @brief Get connection ID
 * @return Connection ID, 0 if not connected
 */
uint16_t ble_app_get_conn_id(void);

/**
 * @brief Start BLE advertisement
 * @param high_duty Use high duty cycle (faster discovery)
 * @return WICED_BT_SUCCESS on success
 */
wiced_result_t ble_app_start_adv(rt_bool_t high_duty);

/**
 * @brief Stop BLE advertisement
 * @return WICED_BT_SUCCESS on success
 */
wiced_result_t ble_app_stop_adv(void);

/**
 * @brief Start BLE scan
 * @param callback Scan result callback
 * @return WICED_BT_SUCCESS on success
 */
wiced_result_t ble_app_start_scan(wiced_bt_ble_scan_result_cback_t *callback);

/**
 * @brief Stop BLE scan
 * @return WICED_BT_SUCCESS on success
 */
wiced_result_t ble_app_stop_scan(void);

/**
 * @brief Send notification data to connected client
 * @param data Data buffer
 * @param len Data length
 * @return WICED_BT_GATT_SUCCESS on success
 */
wiced_bt_gatt_status_t ble_app_send_notification(const uint8_t *data, uint16_t len);

/**
 * @brief Set RX data callback
 * @param callback Callback function
 */
void ble_app_set_rx_callback(ble_app_rx_callback_t callback);

/**
 * @brief Disconnect current connection
 * @return WICED_BT_SUCCESS on success
 */
wiced_result_t ble_app_disconnect(void);

/**
 * @brief Get local Bluetooth address
 * @param addr Output buffer for address
 */
void ble_app_get_local_addr(wiced_bt_device_address_t addr);

/**
 * @brief Get device name
 * @return Device name string
 */
const char* ble_app_get_device_name(void);

/**
 * @brief Get current application state
 * @return Pointer to state structure
 */
const ble_app_state_t* ble_app_get_state(void);

/*******************************************************************************
 * External References (from ble_app_cfg.c)
 ******************************************************************************/
#include "cybt_platform_config.h"

extern const wiced_bt_cfg_settings_t    ble_app_cfg_settings;
extern const uint8_t                    ble_app_gatt_database[];
extern const uint16_t                   ble_app_gatt_database_len;

/* Platform configuration - may be const or non-const depending on pin mode 
 * - WHD_BT_USING_PIN_NAME: Non-const, pins resolved at runtime via rt_pin_get()
 * - WHD_BT_USING_PIN_NUMBER: Const, pins set at compile time from Kconfig
 */
#if defined(WHD_BT_USING_PIN_NAME)
extern cybt_platform_config_t           ble_platform_cfg_settings;  /* Non-const for runtime init */
#else
extern const cybt_platform_config_t     ble_platform_cfg_settings;  /* Const for compile-time init */
#endif

/**
 * @brief Initialize BLE platform configuration
 * 
 * - WHD_BT_USING_PIN_NAME mode: Resolves pin name strings (e.g., "PM.9") 
 *   to actual pin numbers at runtime via rt_pin_get().
 * - WHD_BT_USING_PIN_NUMBER mode: Just prints pin info (already set at compile time).
 * 
 * Must be called before cybt_platform_config_init().
 *
 * @return 0 on success, -1 on pin resolution failure
 */
int ble_platform_cfg_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_APP_H__ */
