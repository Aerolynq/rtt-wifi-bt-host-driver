/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-24     Aerolynq     Initial version - BLE configuration
 *
 * Description:
 *   BLE stack configuration and GATT database definition.
 */

#include <rtthread.h>
#include <rtdevice.h>   /* For rt_pin_get() */
#include "ble_app.h"
#include "wiced_bt_uuid.h"
#include "wiced_bt_gatt.h"
#include "cybt_platform_config.h"
#include "cybsp.h"

/*******************************************************************************
 * Device Name
 ******************************************************************************/
static uint8_t ble_device_name[] = "RTT-BLE";

/*******************************************************************************
 * GATT Database
 * 
 * Use official macros from wiced_bt_gatt.h:
 * - PRIMARY_SERVICE_UUID16/128
 * - CHARACTERISTIC_UUID16/128
 * - CHARACTERISTIC_UUID128_WRITABLE
 * - CHAR_DESCRIPTOR_UUID16_WRITABLE
 ******************************************************************************/

/* GATT Database Definition
 * NOTE: Handle values MUST be in ascending order in the database!
 */
const uint8_t ble_app_gatt_database[] =
{
    /* Declare mandatory GAP service (handle 0x0001) */
    PRIMARY_SERVICE_UUID16(HANDLE_BLE_GAP_SERVICE, UUID_SERVICE_GAP),

    /* Device Name Characteristic */
    CHARACTERISTIC_UUID16(HANDLE_BLE_GAP_DEVICE_NAME,
                          HANDLE_BLE_GAP_DEVICE_NAME_VAL,
                          UUID_CHARACTERISTIC_DEVICE_NAME,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),

    /* Appearance Characteristic */
    CHARACTERISTIC_UUID16(HANDLE_BLE_GAP_APPEARANCE,
                          HANDLE_BLE_GAP_APPEARANCE_VAL,
                          UUID_CHARACTERISTIC_APPEARANCE,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),

    /* Declare mandatory GATT service (handle 0x0010) */
    PRIMARY_SERVICE_UUID16(HANDLE_BLE_GATT_SERVICE, UUID_SERVICE_GATT),

    /* Custom Data Service (128-bit UUID, handle 0x0020) */
    PRIMARY_SERVICE_UUID128(HANDLE_BLE_DATA_SERVICE, UUID_BLE_DATA_SERVICE),

    /* TX Characteristic (Notify) - Device to Phone */
    CHARACTERISTIC_UUID128(HANDLE_BLE_DATA_CHAR_TX,
                           HANDLE_BLE_DATA_CHAR_TX_VAL,
                           UUID_BLE_DATA_CHAR_TX,
                           GATTDB_CHAR_PROP_READ | GATTDB_CHAR_PROP_NOTIFY,
                           GATTDB_PERM_READABLE),

    /* TX CCCD - Client Characteristic Configuration Descriptor */
    CHAR_DESCRIPTOR_UUID16_WRITABLE(HANDLE_BLE_DATA_CHAR_TX_CCCD,
                                    UUID_DESCRIPTOR_CLIENT_CHARACTERISTIC_CONFIGURATION,
                                    GATTDB_PERM_READABLE | GATTDB_PERM_WRITE_REQ),

    /* RX Characteristic (Write) - Phone to Device */
    CHARACTERISTIC_UUID128_WRITABLE(HANDLE_BLE_DATA_CHAR_RX,
                                    HANDLE_BLE_DATA_CHAR_RX_VAL,
                                    UUID_BLE_DATA_CHAR_RX,
                                    GATTDB_CHAR_PROP_WRITE | GATTDB_CHAR_PROP_WRITE_NO_RESPONSE,
                                    GATTDB_PERM_WRITE_CMD | GATTDB_PERM_WRITE_REQ),

    /* Device Information Service */
    PRIMARY_SERVICE_UUID16(HANDLE_BLE_DEV_INFO_SERVICE, UUID_SERVICE_DEVICE_INFORMATION),

    /* Manufacturer Name */
    CHARACTERISTIC_UUID16(HANDLE_BLE_DEV_INFO_MFR_NAME,
                          HANDLE_BLE_DEV_INFO_MFR_NAME_VAL,
                          UUID_CHARACTERISTIC_MANUFACTURER_NAME_STRING,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),

    /* Model Number */
    CHARACTERISTIC_UUID16(HANDLE_BLE_DEV_INFO_MODEL_NUM,
                          HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL,
                          UUID_CHARACTERISTIC_MODEL_NUMBER_STRING,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),

    /* Firmware Version */
    CHARACTERISTIC_UUID16(HANDLE_BLE_DEV_INFO_FW_VER,
                          HANDLE_BLE_DEV_INFO_FW_VER_VAL,
                          UUID_CHARACTERISTIC_FIRMWARE_REVISION_STRING,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),

    /* Battery Service */
    PRIMARY_SERVICE_UUID16(HANDLE_BLE_BATTERY_SERVICE, UUID_SERVICE_BATTERY),

    /* Battery Level */
    CHARACTERISTIC_UUID16(HANDLE_BLE_BATTERY_LEVEL,
                          HANDLE_BLE_BATTERY_LEVEL_VAL,
                          UUID_CHARACTERISTIC_BATTERY_LEVEL,
                          GATTDB_CHAR_PROP_READ,
                          GATTDB_PERM_READABLE),
};

const uint16_t ble_app_gatt_database_len = sizeof(ble_app_gatt_database);

/*******************************************************************************
 * BLE Configuration Settings
 ******************************************************************************/

static wiced_bt_cfg_ble_scan_settings_t ble_scan_cfg =
{
    .scan_mode = BTM_BLE_SCAN_MODE_ACTIVE,
    .high_duty_scan_interval = 96,      /* 60ms */
    .high_duty_scan_window = 48,        /* 30ms */
    .high_duty_scan_duration = 10,      /* 10 seconds */
    .low_duty_scan_interval = 2048,     /* 1.28s */
    .low_duty_scan_window = 48,         /* 30ms */
    .low_duty_scan_duration = 0,        /* infinite */
    .high_duty_conn_scan_interval = 96,
    .high_duty_conn_scan_window = 48,
    .high_duty_conn_duration = 30,
    .low_duty_conn_scan_interval = 2048,
    .low_duty_conn_scan_window = 48,
    .low_duty_conn_duration = 30,
    .conn_min_interval = 12,            /* 15ms */
    .conn_max_interval = 12,            /* 15ms */
    .conn_latency = 0,
    .conn_supervision_timeout = 200,    /* 2 seconds */
};

static const wiced_bt_cfg_ble_advert_settings_t ble_adv_cfg =
{
    .channel_map = BTM_BLE_ADVERT_CHNL_37 | BTM_BLE_ADVERT_CHNL_38 | BTM_BLE_ADVERT_CHNL_39,
    .high_duty_min_interval = 48,       /* 30ms */
    .high_duty_max_interval = 48,
    .high_duty_duration = 0,            /* infinite */
    .low_duty_min_interval = 1024,      /* 640ms */
    .low_duty_max_interval = 1024,
    .low_duty_duration = 0,
    .high_duty_directed_min_interval = 400,
    .high_duty_directed_max_interval = 800,
    .low_duty_directed_min_interval = 400,
    .low_duty_directed_max_interval = 800,
    .low_duty_directed_duration = 30,
    .high_duty_nonconn_min_interval = 160,
    .high_duty_nonconn_max_interval = 160,
    .high_duty_nonconn_duration = 30,
    .low_duty_nonconn_min_interval = 2048,
    .low_duty_nonconn_max_interval = 2048,
    .low_duty_nonconn_duration = 0,
};

static wiced_bt_cfg_ble_t ble_cfg =
{
    .ble_max_simultaneous_links = 2,
    .ble_max_rx_pdu_size = 251,
    .p_ble_scan_cfg = &ble_scan_cfg,
    .p_ble_advert_cfg = &ble_adv_cfg,
    .appearance = 0,                    /* Generic */
    .host_addr_resolution_db_size = 5,
    .rpa_refresh_timeout = 0,           /* Never change */
};

static wiced_bt_cfg_gatt_t gatt_cfg =
{
    .max_db_service_modules = 0,
    .max_eatt_bearers = 0,
};

static wiced_bt_cfg_l2cap_application_t l2cap_cfg =
{
    .max_app_l2cap_psms = 0,
    .max_app_l2cap_channels = 0,
    .max_app_l2cap_le_fixed_channels = 0,
    .max_app_l2cap_br_edr_ertm_chnls = 0,
    .max_app_l2cap_br_edr_ertm_tx_win = 0,
};

const wiced_bt_cfg_settings_t ble_app_cfg_settings =
{
    .device_name = ble_device_name,
    .p_ble_cfg = &ble_cfg,
    .p_gatt_cfg = &gatt_cfg,
    .p_l2cap_app_cfg = &l2cap_cfg,
};

/*******************************************************************************
 * Platform Configuration (HCI UART)
 * 
 * All BT UART settings are now configured via Kconfig (menuconfig):
 *   - BT_UART_DEVICE_NAME: UART device name (e.g., "uart7")
 *   - BT_UART_TX_PIN_NAME/NUMBER: TX pin
 *   - BT_UART_RX_PIN_NAME/NUMBER: RX pin  
 *   - BT_UART_RTS_PIN_NAME/NUMBER: RTS pin (hardware flow control)
 *   - BT_UART_CTS_PIN_NAME/NUMBER: CTS pin (hardware flow control)
 *   - BT_UART_BAUD_RATE_FW_DOWNLOAD: Baud rate for firmware download (115200)
 *   - BT_UART_BAUD_RATE_FEATURE: Baud rate after init (115200~3000000)
 ******************************************************************************/

/* Baud rates from Kconfig */
#ifdef BT_UART_BAUD_RATE_FW_DOWNLOAD
#define CYBT_BAUD_RATE_FOR_FW_DOWNLOAD  (BT_UART_BAUD_RATE_FW_DOWNLOAD)
#else
#define CYBT_BAUD_RATE_FOR_FW_DOWNLOAD  (115200)
#endif

#ifdef BT_UART_BAUD_RATE_FEATURE
#define CYBT_BAUD_RATE_FOR_FEATURE      (BT_UART_BAUD_RATE_FEATURE)
#else
#define CYBT_BAUD_RATE_FOR_FEATURE      (115200)
#endif

/*
 * Get UART pins from Kconfig:
 * - WHD_BT_USING_PIN_NAME: Use string pin names (e.g., "PM.9"), resolved at runtime
 * - WHD_BT_USING_PIN_NUMBER: Use numeric pin numbers, set at compile time
 */
#if defined(WHD_BT_USING_PIN_NAME)

/* Pin names mode: resolve at runtime via rt_pin_get() */
cybt_platform_config_t ble_platform_cfg_settings =
{
    .hci_config =
    {
        .hci_transport = CYBT_HCI_UART,
        .hci =
        {
            .hci_uart =
            {
                /* These will be filled by ble_platform_cfg_init() */
                .uart_tx_pin = 0xFFFF,
                .uart_rx_pin = 0xFFFF,
                .uart_rts_pin = 0xFFFF,
                .uart_cts_pin = 0xFFFF,
                .baud_rate_for_fw_download = CYBT_BAUD_RATE_FOR_FW_DOWNLOAD,
                .baud_rate_for_feature = CYBT_BAUD_RATE_FOR_FEATURE,
                .data_bits = 8,
                .stop_bits = 0,         /* STOP_BITS_1 = 0 in RT-Thread */
                .parity = CYHAL_UART_PARITY_NONE,
                .flow_control = WICED_TRUE,
            }
        }
    },
    .controller_config =
    {
        /* Will be filled by ble_platform_cfg_init() */
        .bt_power_pin = 0xFFFF,
        .sleep_mode =
        {
#ifdef BT_SLEEP_MODE_ENABLED
            .sleep_mode_enabled = WICED_TRUE,
#else
            .sleep_mode_enabled = WICED_FALSE,
#endif
            .device_wakeup_pin = 0xFFFF,
            .host_wakeup_pin = 0xFFFF,
            /* CYW43xxx wake polarity is fixed to Active Low */
            .device_wake_polarity = CYBT_WAKE_ACTIVE_LOW,
            .host_wake_polarity = CYBT_WAKE_ACTIVE_LOW,
        }
    },
    .task_mem_pool_size = 2048,
};

/**
 * @brief Initialize BLE platform configuration with runtime-resolved pins
 *
 * This function must be called before cybt_platform_config_init() when using
 * Kconfig pin names (WHD_BT_USING_PIN_NAME enabled). It converts the pin name
 * strings from Kconfig to actual pin numbers using rt_pin_get().
 *
 * @return 0 on success, -1 on failure
 */
int ble_platform_cfg_init(void)
{
    rt_base_t tx_pin, rx_pin, rts_pin, cts_pin;
    
    /* Resolve pin names to numbers at runtime */
    tx_pin  = rt_pin_get(BT_UART_TX_PIN_NAME);
    rx_pin  = rt_pin_get(BT_UART_RX_PIN_NAME);
    rts_pin = rt_pin_get(BT_UART_RTS_PIN_NAME);
    cts_pin = rt_pin_get(BT_UART_CTS_PIN_NAME);
    
    if (tx_pin < 0 || rx_pin < 0 || rts_pin < 0 || cts_pin < 0)
    {
        rt_kprintf("[BLE] Error: Failed to resolve UART pin names\n");
        rt_kprintf("  TX=%s(%d), RX=%s(%d), RTS=%s(%d), CTS=%s(%d)\n",
                   BT_UART_TX_PIN_NAME, tx_pin,
                   BT_UART_RX_PIN_NAME, rx_pin,
                   BT_UART_RTS_PIN_NAME, rts_pin,
                   BT_UART_CTS_PIN_NAME, cts_pin);
        return -1;
    }
    
    /* Update the platform configuration with resolved pins */
    ble_platform_cfg_settings.hci_config.hci.hci_uart.uart_tx_pin  = tx_pin;
    ble_platform_cfg_settings.hci_config.hci.hci_uart.uart_rx_pin  = rx_pin;
    ble_platform_cfg_settings.hci_config.hci.hci_uart.uart_rts_pin = rts_pin;
    ble_platform_cfg_settings.hci_config.hci.hci_uart.uart_cts_pin = cts_pin;
    
    /* Resolve BT control pins at runtime (check for non-empty strings) */
    rt_base_t power_pin = NC;
    rt_base_t dev_wake_pin = NC;
    rt_base_t host_wake_pin = NC;
    
#ifdef BT_REG_ON_PIN_NAME
    if (BT_REG_ON_PIN_NAME[0] != '\0')
        power_pin = rt_pin_get(BT_REG_ON_PIN_NAME);
#endif
#ifdef BT_DEV_WAKE_PIN_NAME
    if (BT_DEV_WAKE_PIN_NAME[0] != '\0')
        dev_wake_pin = rt_pin_get(BT_DEV_WAKE_PIN_NAME);
#endif
#ifdef BT_HOST_WAKE_PIN_NAME
    if (BT_HOST_WAKE_PIN_NAME[0] != '\0')
        host_wake_pin = rt_pin_get(BT_HOST_WAKE_PIN_NAME);
#endif
    
    ble_platform_cfg_settings.controller_config.bt_power_pin = power_pin;
    ble_platform_cfg_settings.controller_config.sleep_mode.device_wakeup_pin = dev_wake_pin;
    ble_platform_cfg_settings.controller_config.sleep_mode.host_wakeup_pin = host_wake_pin;
    
    rt_kprintf("[BLE] Pins configured via Kconfig (PIN_NAME mode):\n");
    rt_kprintf("  UART: TX=%s(%d), RX=%s(%d), RTS=%s(%d), CTS=%s(%d)\n",
               BT_UART_TX_PIN_NAME, tx_pin,
               BT_UART_RX_PIN_NAME, rx_pin,
               BT_UART_RTS_PIN_NAME, rts_pin,
               BT_UART_CTS_PIN_NAME, cts_pin);
    rt_kprintf("  CTRL: POWER=%d, DEV_WAKE=%d, HOST_WAKE=%d\n",
               power_pin, dev_wake_pin, host_wake_pin);
    
    return 0;
}

#elif defined(WHD_BT_USING_PIN_NUMBER)

/* Pin number mode: use Kconfig numeric values directly at compile time */
const cybt_platform_config_t ble_platform_cfg_settings =
{
    .hci_config =
    {
        .hci_transport = CYBT_HCI_UART,
        .hci =
        {
            .hci_uart =
            {
                .uart_tx_pin = BT_UART_TX_PIN,
                .uart_rx_pin = BT_UART_RX_PIN,
                .uart_rts_pin = BT_UART_RTS_PIN,
                .uart_cts_pin = BT_UART_CTS_PIN,
                .baud_rate_for_fw_download = CYBT_BAUD_RATE_FOR_FW_DOWNLOAD,
                .baud_rate_for_feature = CYBT_BAUD_RATE_FOR_FEATURE,
                .data_bits = 8,
                .stop_bits = 0,         /* STOP_BITS_1 = 0 in RT-Thread */
                .parity = CYHAL_UART_PARITY_NONE,
                .flow_control = WICED_TRUE,
            }
        }
    },
    .controller_config =
    {
#if defined(BT_REG_ON_PIN) && (BT_REG_ON_PIN >= 0)
        .bt_power_pin = BT_REG_ON_PIN,
#else
        .bt_power_pin = NC,
#endif
        .sleep_mode =
        {
#ifdef BT_SLEEP_MODE_ENABLED
            .sleep_mode_enabled = WICED_TRUE,
#else
            .sleep_mode_enabled = WICED_FALSE,
#endif
#if defined(BT_DEV_WAKE_PIN) && (BT_DEV_WAKE_PIN >= 0)
            .device_wakeup_pin = BT_DEV_WAKE_PIN,
#else
            .device_wakeup_pin = NC,
#endif
#if defined(BT_HOST_WAKE_PIN) && (BT_HOST_WAKE_PIN >= 0)
            .host_wakeup_pin = BT_HOST_WAKE_PIN,
#else
            .host_wakeup_pin = NC,
#endif
            /* CYW43xxx wake polarity is fixed to Active Low */
            .device_wake_polarity = CYBT_WAKE_ACTIVE_LOW,
            .host_wake_polarity = CYBT_WAKE_ACTIVE_LOW,
        }
    },
    .task_mem_pool_size = 2048,
};

/**
 * @brief Init function for PIN_NUMBER mode
 * Pins are configured at compile time, just print info.
 */
int ble_platform_cfg_init(void)
{
    rt_kprintf("[BLE] Pins configured via Kconfig (PIN_NUMBER mode):\n");
    rt_kprintf("  UART: TX=%d, RX=%d, RTS=%d, CTS=%d\n",
               BT_UART_TX_PIN, BT_UART_RX_PIN, 
               BT_UART_RTS_PIN, BT_UART_CTS_PIN);
#if defined(BT_REG_ON_PIN)
    rt_kprintf("  CTRL: POWER=%d", BT_REG_ON_PIN);
#else
    rt_kprintf("  CTRL: POWER=NC");
#endif
#if defined(BT_DEV_WAKE_PIN)
    rt_kprintf(", DEV_WAKE=%d", BT_DEV_WAKE_PIN);
#else
    rt_kprintf(", DEV_WAKE=NC");
#endif
#if defined(BT_HOST_WAKE_PIN)
    rt_kprintf(", HOST_WAKE=%d\n", BT_HOST_WAKE_PIN);
#else
    rt_kprintf(", HOST_WAKE=NC\n");
#endif
    return 0;
}

#else
#error "BT UART pins not configured! Enable WHD_BT_USING_PIN_NAME or WHD_BT_USING_PIN_NUMBER in Kconfig."
#endif /* WHD_BT_USING_PIN_NAME / WHD_BT_USING_PIN_NUMBER */
