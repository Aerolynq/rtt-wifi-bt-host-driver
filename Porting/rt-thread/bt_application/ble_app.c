/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-24     Aerolynq     Initial version - BLE peripheral application
 *
 * Description:
 *   This is a clean-room BLE peripheral implementation for RT-Thread.
 *   It provides a simple data transfer service that allows:
 *   - Phone to discover and connect via BLE
 *   - Bidirectional data transfer (TX via notify, RX via write)
 *   - Battery level reporting
 *   - Device information service
 */

#include <rtthread.h>
#include <string.h>

#include "ble_app.h"
#include "wiced_bt_stack.h"
#include "wiced_memory.h"
#include "wiced_bt_dev.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_gatt.h"
#include "wiced_bt_uuid.h"

#define DBG_TAG    "ble.app"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/*******************************************************************************
 * Constants
 ******************************************************************************/
#define BLE_APP_HEAP_SIZE           0x1000

/*******************************************************************************
 * Static Variables
 ******************************************************************************/

/* Application state */
static ble_app_state_t ble_state = {0};
static rt_bool_t ble_initialized = RT_FALSE;
static rt_bool_t gatt_initialized = RT_FALSE;

/* RX callback */
static ble_app_rx_callback_t rx_callback = RT_NULL;

/* TX data buffer */
static uint8_t tx_data_buffer[BLE_APP_MAX_DATA_LEN] = "RTT-BLE";
static uint16_t tx_data_len = 7;

/* RX long write support (Prepare Write / Execute Write) */
static uint8_t rx_data_buffer[BLE_APP_MAX_DATA_LEN];
static uint16_t rx_data_len = 0;

/* Device info strings */
static const char mfr_name[] = "RT-Thread";
static const char model_num[] = "CYW43012";
static const char fw_version[] = "1.0.0";

/*******************************************************************************
 * Forward Declarations
 ******************************************************************************/
static wiced_result_t ble_app_management_callback(wiced_bt_management_evt_t event,
                                                   wiced_bt_management_evt_data_t *p_event_data);
static wiced_bt_gatt_status_t ble_app_gatt_callback(wiced_bt_gatt_evt_t event,
                                                     wiced_bt_gatt_event_data_t *p_data);
static void ble_app_set_advertisement_data(void);
static void ble_app_stack_enabled(void);

static wiced_bt_gatt_status_t ble_app_gatt_read_by_type_handler(uint16_t conn_id,
                                                                wiced_bt_gatt_opcode_t opcode,
                                                                wiced_bt_gatt_read_by_type_t *p_read,
                                                                uint16_t len_requested);
static wiced_bt_gatt_status_t ble_app_gatt_read_multiple_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_read_multiple_req_t *p_read,
                                                                 uint16_t len_requested);
static wiced_bt_gatt_status_t ble_app_gatt_prepare_write_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_write_req_t *p_write);
static wiced_bt_gatt_status_t ble_app_gatt_execute_write_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_execute_write_req_t *p_exec);

static rt_bool_t ble_app_uuid_equal(const wiced_bt_uuid_t *a, const wiced_bt_uuid_t *b);

/*******************************************************************************
 * GATT Read/Write Handlers
 ******************************************************************************/

static wiced_bt_gatt_status_t ble_app_gatt_read_handler(uint16_t conn_id,
                                                         wiced_bt_gatt_opcode_t opcode,
                                                         wiced_bt_gatt_read_t *p_read,
                                                         uint16_t len_requested)
{
    wiced_bt_gatt_status_t status = WICED_BT_GATT_INVALID_HANDLE;
    uint8_t *p_data = NULL;
    uint16_t attr_len = 0;

    switch (p_read->handle)
    {
        case HANDLE_BLE_GAP_DEVICE_NAME_VAL:
            p_data = (uint8_t *)ble_app_cfg_settings.device_name;
            attr_len = strlen((const char *)ble_app_cfg_settings.device_name);
            break;

        case HANDLE_BLE_GAP_APPEARANCE_VAL:
        {
            static uint8_t appearance[] = {0x00, 0x00}; /* Generic */
            p_data = appearance;
            attr_len = sizeof(appearance);
        }
        break;

        case HANDLE_BLE_DATA_CHAR_TX_VAL:
            p_data = tx_data_buffer;
            attr_len = tx_data_len;
            break;

        case HANDLE_BLE_DATA_CHAR_TX_CCCD:
            p_data = (uint8_t *)&ble_state.cccd_flags;
            attr_len = 2;
            break;

        case HANDLE_BLE_DEV_INFO_MFR_NAME_VAL:
            p_data = (uint8_t *)mfr_name;
            attr_len = strlen(mfr_name);
            break;

        case HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL:
            p_data = (uint8_t *)model_num;
            attr_len = strlen(model_num);
            break;

        case HANDLE_BLE_DEV_INFO_FW_VER_VAL:
            p_data = (uint8_t *)fw_version;
            attr_len = strlen(fw_version);
            break;

        case HANDLE_BLE_BATTERY_LEVEL_VAL:
            p_data = &ble_state.battery_level;
            attr_len = 1;
            break;

        default:
            LOG_W("Read: unknown handle 0x%04x", p_read->handle);
            return WICED_BT_GATT_INVALID_HANDLE;
    }

    if (p_data && attr_len > 0)
    {
        uint16_t offset = p_read->offset;
        uint16_t to_copy;

        if (offset >= attr_len)
        {
            return WICED_BT_GATT_INVALID_OFFSET;
        }

        to_copy = attr_len - offset;
        if (to_copy > len_requested)
            to_copy = len_requested;

        uint8_t *p_rsp = wiced_bt_get_buffer(to_copy);
        if (p_rsp == NULL)
        {
            return WICED_BT_GATT_INSUF_RESOURCE;
        }

        memcpy(p_rsp, p_data + offset, to_copy);
        /* Do NOT free p_rsp here: stack transmits asynchronously.
         * Free will be called from GATT_APP_BUFFER_TRANSMITTED_EVT via p_app_ctxt.
         */
        wiced_bt_gatt_server_send_read_handle_rsp(conn_id, opcode, to_copy, p_rsp,
                                                  (wiced_bt_gatt_app_context_t)wiced_bt_free_buffer);
        status = WICED_BT_GATT_SUCCESS;
    }

    return status;
}

static wiced_bt_gatt_status_t ble_app_gatt_write_handler(uint16_t conn_id,
                                                          wiced_bt_gatt_opcode_t opcode,
                                                          wiced_bt_gatt_write_req_t *p_write)
{
    wiced_bt_gatt_status_t status = WICED_BT_GATT_INVALID_HANDLE;

    switch (p_write->handle)
    {
        case HANDLE_BLE_DATA_CHAR_TX_CCCD:
            if (p_write->val_len == 2)
            {
                ble_state.cccd_flags = p_write->p_val[0] | (p_write->p_val[1] << 8);
                LOG_I("CCCD updated: 0x%04x", ble_state.cccd_flags);
                status = WICED_BT_GATT_SUCCESS;
            }
            else
            {
                status = WICED_BT_GATT_INVALID_ATTR_LEN;
            }
            break;

        case HANDLE_BLE_DATA_CHAR_RX_VAL:
            LOG_I("RX data received: %d bytes", p_write->val_len);
            /* Print hex data for debugging */
            if (p_write->val_len > 0)
            {
                rt_kprintf("[ble.app] RX HEX: ");
                for (uint16_t i = 0; i < p_write->val_len && i < 32; i++)
                {
                    rt_kprintf("%02X ", p_write->p_val[i]);
                }
                if (p_write->val_len > 32)
                    rt_kprintf("...");
                rt_kprintf("\n");
            }
            if (rx_callback && p_write->val_len > 0)
            {
                rx_callback(p_write->p_val, p_write->val_len);
            }
            status = WICED_BT_GATT_SUCCESS;
            break;

        default:
            LOG_W("Write: unknown handle 0x%04x", p_write->handle);
            break;
    }

    return status;
}

static rt_bool_t ble_app_uuid_equal(const wiced_bt_uuid_t *a, const wiced_bt_uuid_t *b)
{
    if (a == NULL || b == NULL)
        return RT_FALSE;
    if (a->len != b->len)
        return RT_FALSE;

    switch (a->len)
    {
        case LEN_UUID_16:
            return (a->uu.uuid16 == b->uu.uuid16) ? RT_TRUE : RT_FALSE;
        case LEN_UUID_32:
            return (a->uu.uuid32 == b->uu.uuid32) ? RT_TRUE : RT_FALSE;
        case LEN_UUID_128:
            return (memcmp(a->uu.uuid128, b->uu.uuid128, LEN_UUID_128) == 0) ? RT_TRUE : RT_FALSE;
        default:
            return RT_FALSE;
    }
}

static wiced_bt_gatt_status_t ble_app_gatt_read_by_type_handler(uint16_t conn_id,
                                                                wiced_bt_gatt_opcode_t opcode,
                                                                wiced_bt_gatt_read_by_type_t *p_read,
                                                                uint16_t len_requested)
{
    /* Support common Read By Type requests used during discovery */
    uint8_t *p_rsp = wiced_bt_get_buffer(len_requested);
    uint16_t used = 0;
    uint8_t pair_len = 0;
    uint16_t handle;

    if (p_rsp == NULL)
        return WICED_BT_GATT_INSUF_RESOURCE;

    /* UUID constants */
    const wiced_bt_uuid_t uuid_char_decl = { .len = LEN_UUID_16, .uu.uuid16 = UUID_ATTRIBUTE_CHARACTERISTIC };
    const wiced_bt_uuid_t uuid_dev_name  = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_DEVICE_NAME };
    const wiced_bt_uuid_t uuid_appearance = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_APPEARANCE };
    const wiced_bt_uuid_t uuid_mfr = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_MANUFACTURER_NAME_STRING };
    const wiced_bt_uuid_t uuid_model = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_MODEL_NUMBER_STRING };
    const wiced_bt_uuid_t uuid_fw = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_FIRMWARE_REVISION_STRING };
    const wiced_bt_uuid_t uuid_batt = { .len = LEN_UUID_16, .uu.uuid16 = UUID_CHARACTERISTIC_BATTERY_LEVEL };

    /* Characteristic declaration values (properties + value_handle + uuid) */
    static const uint8_t gap_dev_name_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_GAP_DEVICE_NAME_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_DEVICE_NAME),
    };
    static const uint8_t gap_appearance_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_GAP_APPEARANCE_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_APPEARANCE),
    };
    static const uint8_t data_tx_decl[] =
    {
        (uint8_t)(GATTDB_CHAR_PROP_READ | GATTDB_CHAR_PROP_NOTIFY),
        BIT16_TO_8((uint16_t)HANDLE_BLE_DATA_CHAR_TX_VAL),
        UUID_BLE_DATA_CHAR_TX,
    };
    static const uint8_t data_rx_decl[] =
    {
        (uint8_t)(GATTDB_CHAR_PROP_WRITE | GATTDB_CHAR_PROP_WRITE_NO_RESPONSE),
        BIT16_TO_8((uint16_t)HANDLE_BLE_DATA_CHAR_RX_VAL),
        UUID_BLE_DATA_CHAR_RX,
    };
    static const uint8_t dis_mfr_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_DEV_INFO_MFR_NAME_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_MANUFACTURER_NAME_STRING),
    };
    static const uint8_t dis_model_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_MODEL_NUMBER_STRING),
    };
    static const uint8_t dis_fw_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_DEV_INFO_FW_VER_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_FIRMWARE_REVISION_STRING),
    };
    static const uint8_t batt_level_decl[] =
    {
        (uint8_t)GATTDB_CHAR_PROP_READ,
        BIT16_TO_8((uint16_t)HANDLE_BLE_BATTERY_LEVEL_VAL),
        BIT16_TO_8((uint16_t)UUID_CHARACTERISTIC_BATTERY_LEVEL),
    };

    /* Build response by iterating known handles in range */
    for (handle = p_read->s_handle; handle <= p_read->e_handle; handle++)
    {
        const uint8_t *p_attr = NULL;
        uint16_t attr_len = 0;

        if (ble_app_uuid_equal(&p_read->uuid, &uuid_char_decl))
        {
            switch (handle)
            {
                case HANDLE_BLE_GAP_DEVICE_NAME:      p_attr = gap_dev_name_decl;   attr_len = sizeof(gap_dev_name_decl); break;
                case HANDLE_BLE_GAP_APPEARANCE:       p_attr = gap_appearance_decl; attr_len = sizeof(gap_appearance_decl); break;
                case HANDLE_BLE_DATA_CHAR_TX:         p_attr = data_tx_decl;        attr_len = sizeof(data_tx_decl); break;
                case HANDLE_BLE_DATA_CHAR_RX:         p_attr = data_rx_decl;        attr_len = sizeof(data_rx_decl); break;
                case HANDLE_BLE_DEV_INFO_MFR_NAME:    p_attr = dis_mfr_decl;        attr_len = sizeof(dis_mfr_decl); break;
                case HANDLE_BLE_DEV_INFO_MODEL_NUM:   p_attr = dis_model_decl;      attr_len = sizeof(dis_model_decl); break;
                case HANDLE_BLE_DEV_INFO_FW_VER:      p_attr = dis_fw_decl;         attr_len = sizeof(dis_fw_decl); break;
                case HANDLE_BLE_BATTERY_LEVEL:        p_attr = batt_level_decl;     attr_len = sizeof(batt_level_decl); break;
                default: break;
            }
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_dev_name) && handle == HANDLE_BLE_GAP_DEVICE_NAME_VAL)
        {
            p_attr = (const uint8_t *)ble_app_cfg_settings.device_name;
            attr_len = strlen((const char *)ble_app_cfg_settings.device_name);
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_appearance) && handle == HANDLE_BLE_GAP_APPEARANCE_VAL)
        {
            static const uint8_t appearance[] = {0x00, 0x00};
            p_attr = appearance;
            attr_len = sizeof(appearance);
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_mfr) && handle == HANDLE_BLE_DEV_INFO_MFR_NAME_VAL)
        {
            p_attr = (const uint8_t *)mfr_name;
            attr_len = strlen(mfr_name);
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_model) && handle == HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL)
        {
            p_attr = (const uint8_t *)model_num;
            attr_len = strlen(model_num);
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_fw) && handle == HANDLE_BLE_DEV_INFO_FW_VER_VAL)
        {
            p_attr = (const uint8_t *)fw_version;
            attr_len = strlen(fw_version);
        }
        else if (ble_app_uuid_equal(&p_read->uuid, &uuid_batt) && handle == HANDLE_BLE_BATTERY_LEVEL_VAL)
        {
            p_attr = &ble_state.battery_level;
            attr_len = 1;
        }

        if (p_attr && attr_len)
        {
            int filled = wiced_bt_gatt_put_read_by_type_rsp_in_stream(p_rsp + used,
                                                                       (int)(len_requested - used),
                                                                       &pair_len,
                                                                       handle,
                                                                       attr_len,
                                                                       p_attr);
            if (filled <= 0)
                break;
            used += (uint16_t)filled;
        }
    }

    if (used == 0)
    {
        wiced_bt_free_buffer(p_rsp);
        wiced_bt_gatt_server_send_error_rsp(conn_id, opcode, p_read->s_handle, WICED_BT_GATT_ATTRIBUTE_NOT_FOUND);
        return WICED_BT_GATT_ATTRIBUTE_NOT_FOUND;
    }

    return wiced_bt_gatt_server_send_read_by_type_rsp(conn_id,
                                                      opcode,
                                                      pair_len,
                                                      used,
                                                      p_rsp,
                                                      (wiced_bt_gatt_app_context_t)wiced_bt_free_buffer);
}

static wiced_bt_gatt_status_t ble_app_gatt_read_multiple_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_read_multiple_req_t *p_read,
                                                                 uint16_t len_requested)
{
    uint8_t *p_rsp = wiced_bt_get_buffer(len_requested);
    uint16_t used = 0;

    if (p_rsp == NULL)
        return WICED_BT_GATT_INSUF_RESOURCE;

    for (int i = 0; i < p_read->num_handles; i++)
    {
        uint16_t handle = wiced_bt_gatt_get_handle_from_stream(p_read->p_handle_stream, (uint16_t)i);
        const uint8_t *p_attr = NULL;
        uint16_t attr_len = 0;

        switch (handle)
        {
            case HANDLE_BLE_GAP_DEVICE_NAME_VAL:
                p_attr = (const uint8_t *)ble_app_cfg_settings.device_name;
                attr_len = strlen((const char *)ble_app_cfg_settings.device_name);
                break;
            case HANDLE_BLE_GAP_APPEARANCE_VAL:
            {
                static const uint8_t appearance[] = {0x00, 0x00};
                p_attr = appearance;
                attr_len = sizeof(appearance);
                break;
            }
            case HANDLE_BLE_DATA_CHAR_TX_VAL:
                p_attr = tx_data_buffer;
                attr_len = tx_data_len;
                break;
            case HANDLE_BLE_DATA_CHAR_TX_CCCD:
                p_attr = (const uint8_t *)&ble_state.cccd_flags;
                attr_len = 2;
                break;
            case HANDLE_BLE_DEV_INFO_MFR_NAME_VAL:
                p_attr = (const uint8_t *)mfr_name;
                attr_len = strlen(mfr_name);
                break;
            case HANDLE_BLE_DEV_INFO_MODEL_NUM_VAL:
                p_attr = (const uint8_t *)model_num;
                attr_len = strlen(model_num);
                break;
            case HANDLE_BLE_DEV_INFO_FW_VER_VAL:
                p_attr = (const uint8_t *)fw_version;
                attr_len = strlen(fw_version);
                break;
            case HANDLE_BLE_BATTERY_LEVEL_VAL:
                p_attr = &ble_state.battery_level;
                attr_len = 1;
                break;
            default:
                break;
        }

        if (p_attr && attr_len)
        {
            int filled = wiced_bt_gatt_put_read_multi_rsp_in_stream(opcode,
                                                                    p_rsp + used,
                                                                    (int)(len_requested - used),
                                                                    handle,
                                                                    attr_len,
                                                                    p_attr);
            if (filled <= 0)
                break;
            used += (uint16_t)filled;
        }
        else
        {
            /* If any handle is invalid, respond with error */
            wiced_bt_free_buffer(p_rsp);
            wiced_bt_gatt_server_send_error_rsp(conn_id, opcode, handle, WICED_BT_GATT_INVALID_HANDLE);
            return WICED_BT_GATT_INVALID_HANDLE;
        }
    }

    return wiced_bt_gatt_server_send_read_multiple_rsp(conn_id,
                                                       opcode,
                                                       used,
                                                       p_rsp,
                                                       (wiced_bt_gatt_app_context_t)wiced_bt_free_buffer);
}

static wiced_bt_gatt_status_t ble_app_gatt_prepare_write_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_write_req_t *p_write)
{
    /* Minimal Prepare Write support for RX value (long writes). */
    if (p_write->handle != HANDLE_BLE_DATA_CHAR_RX_VAL)
    {
        wiced_bt_gatt_server_send_error_rsp(conn_id, opcode, p_write->handle, WICED_BT_GATT_INVALID_HANDLE);
        return WICED_BT_GATT_INVALID_HANDLE;
    }

    if ((p_write->offset + p_write->val_len) > BLE_APP_MAX_DATA_LEN)
    {
        wiced_bt_gatt_server_send_error_rsp(conn_id, opcode, p_write->handle, WICED_BT_GATT_INVALID_ATTR_LEN);
        return WICED_BT_GATT_INVALID_ATTR_LEN;
    }

    memcpy(&rx_data_buffer[p_write->offset], p_write->p_val, p_write->val_len);
    if ((p_write->offset + p_write->val_len) > rx_data_len)
        rx_data_len = p_write->offset + p_write->val_len;

    /* Per API docs, send a COPY of the received data pointer */
    uint8_t *p_rsp_copy = wiced_bt_get_buffer(p_write->val_len);
    if (p_rsp_copy == NULL)
        return WICED_BT_GATT_INSUF_RESOURCE;
    memcpy(p_rsp_copy, p_write->p_val, p_write->val_len);

    return wiced_bt_gatt_server_send_prepare_write_rsp(conn_id,
                                                       opcode,
                                                       p_write->handle,
                                                       p_write->offset,
                                                       p_write->val_len,
                                                       p_rsp_copy,
                                                       (wiced_bt_gatt_app_context_t)wiced_bt_free_buffer);
}

static wiced_bt_gatt_status_t ble_app_gatt_execute_write_handler(uint16_t conn_id,
                                                                 wiced_bt_gatt_opcode_t opcode,
                                                                 wiced_bt_gatt_execute_write_req_t *p_exec)
{
    if (p_exec->exec_write == GATT_PREPARE_WRITE_EXEC)
    {
        LOG_I("Execute write: %d bytes", rx_data_len);
        if (rx_data_len > 0)
        {
            rt_kprintf("[ble.app] RX HEX (exec): ");
            for (uint16_t i = 0; i < rx_data_len && i < 32; i++)
            {
                rt_kprintf("%02X ", rx_data_buffer[i]);
            }
            if (rx_data_len > 32)
                rt_kprintf("...");
            rt_kprintf("\n");

            if (rx_callback)
            {
                rx_callback(rx_data_buffer, rx_data_len);
            }
        }
    }

    /* Clear queue on execute or cancel */
    rx_data_len = 0;
    memset(rx_data_buffer, 0, sizeof(rx_data_buffer));

    return wiced_bt_gatt_server_send_execute_write_rsp(conn_id, opcode);
}

/*******************************************************************************
 * GATT Connection Handlers
 ******************************************************************************/

static wiced_bt_gatt_status_t ble_app_gatt_connection_up(wiced_bt_gatt_connection_status_t *p_status)
{
    LOG_I("BLE connected: conn_id=%d", p_status->conn_id);
    LOG_I("  Peer: %02X:%02X:%02X:%02X:%02X:%02X",
          p_status->bd_addr[0], p_status->bd_addr[1], p_status->bd_addr[2],
          p_status->bd_addr[3], p_status->bd_addr[4], p_status->bd_addr[5]);

    ble_state.conn_id = p_status->conn_id;
    memcpy(ble_state.peer_addr, p_status->bd_addr, BD_ADDR_LEN);
    ble_state.is_connected = 1;
    ble_state.cccd_flags = 0;

    /* Stop advertising when connected */
    wiced_bt_start_advertisements(BTM_BLE_ADVERT_OFF, 0, NULL);

    return WICED_BT_GATT_SUCCESS;
}

static wiced_bt_gatt_status_t ble_app_gatt_connection_down(wiced_bt_gatt_connection_status_t *p_status)
{
    LOG_I("BLE disconnected: reason=%d", p_status->reason);

    ble_state.conn_id = 0;
    memset(ble_state.peer_addr, 0, BD_ADDR_LEN);
    ble_state.is_connected = 0;
    ble_state.cccd_flags = 0;

    /* Restart advertising after disconnect */
    wiced_bt_start_advertisements(BTM_BLE_ADVERT_UNDIRECTED_LOW, 0, NULL);

    return WICED_BT_GATT_SUCCESS;
}

static wiced_bt_gatt_status_t ble_app_gatt_conn_status_cb(wiced_bt_gatt_connection_status_t *p_status)
{
    if (p_status->connected)
    {
        return ble_app_gatt_connection_up(p_status);
    }
    return ble_app_gatt_connection_down(p_status);
}

/*******************************************************************************
 * GATT Request Handler
 ******************************************************************************/

static wiced_bt_gatt_status_t ble_app_gatt_req_cb(wiced_bt_gatt_attribute_request_t *p_req)
{
    wiced_bt_gatt_status_t status = WICED_BT_GATT_INVALID_PDU;

    /* Keep key request tracing at INFO to help diagnose disconnects */
    switch (p_req->opcode)
    {
        case GATT_REQ_READ_BY_TYPE:
            LOG_I("GATT read_by_type: s=0x%04x e=0x%04x uuid_len=%d", p_req->data.read_by_type.s_handle,
                  p_req->data.read_by_type.e_handle, p_req->data.read_by_type.uuid.len);
            break;
        case GATT_REQ_READ_MULTI:
        case GATT_REQ_READ_MULTI_VAR_LENGTH:
            LOG_I("GATT read_multi: n=%d", p_req->data.read_multiple_req.num_handles);
            break;
        case GATT_REQ_WRITE:
        case GATT_CMD_WRITE:
        case GATT_REQ_PREPARE_WRITE:
            LOG_I("GATT write: h=0x%04x op=0x%02x off=%d len=%d", p_req->data.write_req.handle, p_req->opcode,
                  p_req->data.write_req.offset, p_req->data.write_req.val_len);
            break;
        case GATT_REQ_EXECUTE_WRITE:
            LOG_I("GATT execute_write: flag=0x%02x", p_req->data.exec_write_req.exec_write);
            break;
        default:
            break;
    }

    switch (p_req->opcode)
    {
        /* These opcodes are generated by the stack for server-originated notifications/indications
         * (not client requests). They should not be treated as errors.
         */
        case GATT_HANDLE_VALUE_NOTIF:
        case GATT_HANDLE_VALUE_MULTI_NOTIF:
            LOG_D("GATT notif sent: first_handle=0x%04x", p_req->data.confirm.handle);
            status = WICED_BT_GATT_SUCCESS;
            break;

        case GATT_HANDLE_VALUE_IND:
            LOG_D("GATT indication sent: first_handle=0x%04x", p_req->data.confirm.handle);
            status = WICED_BT_GATT_SUCCESS;
            break;

        case GATT_HANDLE_VALUE_CONF:
            LOG_D("GATT indication confirmed: first_handle=0x%04x", p_req->data.confirm.handle);
            status = WICED_BT_GATT_SUCCESS;
            break;

        case GATT_REQ_READ:
        case GATT_REQ_READ_BLOB:
            status = ble_app_gatt_read_handler(p_req->conn_id, p_req->opcode,
                                                &p_req->data.read_req, p_req->len_requested);
            if (status != WICED_BT_GATT_SUCCESS)
            {
                wiced_bt_gatt_server_send_error_rsp(p_req->conn_id, p_req->opcode,
                                                    p_req->data.read_req.handle, status);
            }
            break;

        case GATT_REQ_READ_BY_TYPE:
            status = ble_app_gatt_read_by_type_handler(p_req->conn_id, p_req->opcode,
                                                       &p_req->data.read_by_type, p_req->len_requested);
            break;

        case GATT_REQ_READ_MULTI:
        case GATT_REQ_READ_MULTI_VAR_LENGTH:
            status = ble_app_gatt_read_multiple_handler(p_req->conn_id, p_req->opcode,
                                                        &p_req->data.read_multiple_req, p_req->len_requested);
            break;

        case GATT_REQ_WRITE:
        case GATT_CMD_WRITE:
            status = ble_app_gatt_write_handler(p_req->conn_id, p_req->opcode,
                                                 &p_req->data.write_req);
            if (p_req->opcode == GATT_REQ_WRITE && status == WICED_BT_GATT_SUCCESS)
            {
                wiced_bt_gatt_server_send_write_rsp(p_req->conn_id, p_req->opcode,
                                                    p_req->data.write_req.handle);
            }
            else if (p_req->opcode == GATT_REQ_WRITE && status != WICED_BT_GATT_SUCCESS)
            {
                wiced_bt_gatt_server_send_error_rsp(p_req->conn_id, p_req->opcode,
                                                    p_req->data.write_req.handle, status);
            }
            break;

        case GATT_REQ_PREPARE_WRITE:
            status = ble_app_gatt_prepare_write_handler(p_req->conn_id, p_req->opcode,
                                                        &p_req->data.write_req);
            break;

        case GATT_REQ_EXECUTE_WRITE:
            status = ble_app_gatt_execute_write_handler(p_req->conn_id, p_req->opcode,
                                                        &p_req->data.exec_write_req);
            break;

        case GATT_REQ_MTU:
            ble_state.peer_mtu = p_req->data.remote_mtu;
            LOG_I("MTU request: %d", ble_state.peer_mtu);
            /* Use ble_max_rx_pdu_size from config (251) as our MTU */
            wiced_bt_gatt_server_send_mtu_rsp(p_req->conn_id, p_req->data.remote_mtu,
                                              ble_app_cfg_settings.p_ble_cfg->ble_max_rx_pdu_size);
            status = WICED_BT_GATT_SUCCESS;
            break;

        default:
            LOG_W("Unhandled GATT opcode: 0x%02x", p_req->opcode);
            wiced_bt_gatt_server_send_error_rsp(p_req->conn_id, p_req->opcode, 0, WICED_BT_GATT_REQ_NOT_SUPPORTED);
            status = WICED_BT_GATT_REQ_NOT_SUPPORTED;
            break;
    }

    return status;
}

/*******************************************************************************
 * GATT Callback
 ******************************************************************************/

static wiced_bt_gatt_status_t ble_app_gatt_callback(wiced_bt_gatt_evt_t event,
                                                     wiced_bt_gatt_event_data_t *p_data)
{
    wiced_bt_gatt_status_t status = WICED_BT_GATT_SUCCESS;

    switch (event)
    {
        case GATT_CONNECTION_STATUS_EVT:
            status = ble_app_gatt_conn_status_cb(&p_data->connection_status);
            break;

        case GATT_ATTRIBUTE_REQUEST_EVT:
            status = ble_app_gatt_req_cb(&p_data->attribute_request);
            break;

        case GATT_GET_RESPONSE_BUFFER_EVT:
            p_data->buffer_request.buffer.p_app_rsp_buffer =
                wiced_bt_get_buffer(p_data->buffer_request.len_requested);
            p_data->buffer_request.buffer.p_app_ctxt = (wiced_bt_gatt_app_context_t)wiced_bt_free_buffer;
            break;

        case GATT_APP_BUFFER_TRANSMITTED_EVT:
            if (p_data->buffer_xmitted.p_app_ctxt)
            {
                ((void (*)(uint8_t *))p_data->buffer_xmitted.p_app_ctxt)(p_data->buffer_xmitted.p_app_data);
            }
            break;

        default:
            break;
    }

    return status;
}

/*******************************************************************************
 * Advertisement Data Setup
 ******************************************************************************/

static void ble_app_set_advertisement_data(void)
{
    wiced_bt_ble_advert_elem_t adv_elem[3];
    uint8_t num_elem = 0;
    uint8_t flags = BTM_BLE_GENERAL_DISCOVERABLE_FLAG | BTM_BLE_BREDR_NOT_SUPPORTED;
    uint8_t service_uuid128[] = {UUID_BLE_DATA_SERVICE};

    /* Flags */
    adv_elem[num_elem].advert_type = BTM_BLE_ADVERT_TYPE_FLAG;
    adv_elem[num_elem].len = sizeof(flags);
    adv_elem[num_elem].p_data = &flags;
    num_elem++;

    /* 128-bit Service UUID */
    adv_elem[num_elem].advert_type = BTM_BLE_ADVERT_TYPE_128SRV_COMPLETE;
    adv_elem[num_elem].len = sizeof(service_uuid128);
    adv_elem[num_elem].p_data = service_uuid128;
    num_elem++;

    /* Device name */
    adv_elem[num_elem].advert_type = BTM_BLE_ADVERT_TYPE_NAME_COMPLETE;
    adv_elem[num_elem].len = strlen((const char *)ble_app_cfg_settings.device_name);
    adv_elem[num_elem].p_data = (uint8_t *)ble_app_cfg_settings.device_name;
    num_elem++;

    wiced_bt_ble_set_raw_advertisement_data(num_elem, adv_elem);
}

/*******************************************************************************
 * Stack Enabled Callback (BTM_ENABLED_EVT)
 * Only minimal initialization here - GATT init is done via ble_app_gatt_init()
 ******************************************************************************/

static void ble_app_stack_enabled(void)
{
    wiced_bt_device_address_t bda;

    /* Print local address */
    wiced_bt_dev_read_local_addr(bda);
    LOG_I("Local BLE Address: %02X:%02X:%02X:%02X:%02X:%02X",
          bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    LOG_I("BT stack ready. Use 'ble gatt start' to init GATT, then 'ble adv start' to advertise.");
}

/*******************************************************************************
 * Management Callback
 ******************************************************************************/

static wiced_result_t ble_app_management_callback(wiced_bt_management_evt_t event,
                                                   wiced_bt_management_evt_data_t *p_event_data)
{
    wiced_result_t result = WICED_BT_SUCCESS;

    LOG_D("BT management event: 0x%x", event);

    switch (event)
    {
        case BTM_ENABLED_EVT:
            if (p_event_data->enabled.status == WICED_BT_SUCCESS)
            {
                LOG_I("Bluetooth stack enabled");
                ble_app_stack_enabled();
            }
            else
            {
                LOG_E("Bluetooth enable failed: %d", p_event_data->enabled.status);
            }
            break;

        case BTM_DISABLED_EVT:
            LOG_I("Bluetooth disabled");
            break;

        case BTM_PAIRING_IO_CAPABILITIES_BLE_REQUEST_EVT:
            p_event_data->pairing_io_capabilities_ble_request.local_io_cap = BTM_IO_CAPABILITIES_NONE;
            p_event_data->pairing_io_capabilities_ble_request.oob_data = BTM_OOB_NONE;
            p_event_data->pairing_io_capabilities_ble_request.auth_req = BTM_LE_AUTH_REQ_SC_BOND;
            p_event_data->pairing_io_capabilities_ble_request.max_key_size = 16;
            p_event_data->pairing_io_capabilities_ble_request.init_keys =
                BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
            p_event_data->pairing_io_capabilities_ble_request.resp_keys =
                BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
            break;

        case BTM_PAIRING_COMPLETE_EVT:
            LOG_I("Pairing complete: %d",
                  p_event_data->pairing_complete.pairing_complete_info.ble.reason);
            break;

        case BTM_PAIRED_DEVICE_LINK_KEYS_REQUEST_EVT:
            /* No stored keys */
            result = WICED_BT_UNKNOWN_ADDR;
            break;

        case BTM_LOCAL_IDENTITY_KEYS_REQUEST_EVT:
            /* Generate new keys */
            result = WICED_BT_NO_RESOURCES;
            break;

        case BTM_BLE_ADVERT_STATE_CHANGED_EVT:
            LOG_D("ADV state: %d", p_event_data->ble_advert_state_changed);
            break;

        case BTM_ENCRYPTION_STATUS_EVT:
            LOG_D("Encryption status: %d", p_event_data->encryption_status.result);
            break;

        case BTM_SECURITY_REQUEST_EVT:
            wiced_bt_ble_security_grant(p_event_data->security_request.bd_addr, WICED_BT_SUCCESS);
            break;

        case BTM_USER_CONFIRMATION_REQUEST_EVT:
            wiced_bt_dev_confirm_req_reply(WICED_BT_SUCCESS,
                                           p_event_data->user_confirmation_request.bd_addr);
            break;

        default:
            LOG_D("Unhandled BT event: 0x%x", event);
            break;
    }

    return result;
}

/*******************************************************************************
 * Public API Implementation
 ******************************************************************************/

rt_err_t ble_app_init(void)
{
    wiced_result_t result;

    if (ble_initialized)
    {
        LOG_W("BLE already initialized");
        return RT_EOK;
    }

    LOG_I("Initializing BLE application...");

    /* Initialize BT stack */
    result = wiced_bt_stack_init(ble_app_management_callback, &ble_app_cfg_settings);
    if (result != WICED_BT_SUCCESS)
    {
        LOG_E("BT stack init failed: %d", result);
        return -RT_ERROR;
    }

    /* Create application heap */
    wiced_bt_create_heap("ble_app", NULL, BLE_APP_HEAP_SIZE, NULL, WICED_TRUE);

    ble_initialized = RT_TRUE;
    return RT_EOK;
}

rt_err_t ble_app_gatt_init(void)
{
    wiced_bt_gatt_status_t gatt_status;

    if (!ble_initialized)
    {
        LOG_E("BT stack not initialized");
        return -RT_ERROR;
    }

    if (gatt_initialized)
    {
        LOG_W("GATT already initialized");
        return RT_EOK;
    }

    LOG_I("Initializing BLE GATT...");

    /* Register GATT callback */
    gatt_status = wiced_bt_gatt_register(ble_app_gatt_callback);
    if (gatt_status != WICED_BT_GATT_SUCCESS)
    {
        LOG_E("GATT register failed: %d", gatt_status);
        return -RT_ERROR;
    }

    /* Initialize GATT database */
    gatt_status = wiced_bt_gatt_db_init(ble_app_gatt_database, ble_app_gatt_database_len, NULL);
    if (gatt_status != WICED_BT_GATT_SUCCESS)
    {
        LOG_E("GATT DB init failed: %d", gatt_status);
        return -RT_ERROR;
    }

    /* Initialize state */
    memset(&ble_state, 0, sizeof(ble_state));
    ble_state.battery_level = 100;

    /* Allow pairing */
    wiced_bt_set_pairable_mode(WICED_TRUE, 0);

    /* Setup advertisement data */
    ble_app_set_advertisement_data();

    gatt_initialized = RT_TRUE;
    LOG_I("GATT initialized. Device: %s", ble_app_cfg_settings.device_name);
    LOG_I("Use 'ble adv start' to begin advertising.");

    return RT_EOK;
}

rt_bool_t ble_app_is_gatt_initialized(void)
{
    return gatt_initialized;
}

rt_bool_t ble_app_is_initialized(void)
{
    return ble_initialized;
}

rt_bool_t ble_app_is_connected(void)
{
    return ble_state.is_connected ? RT_TRUE : RT_FALSE;
}

uint16_t ble_app_get_conn_id(void)
{
    return ble_state.conn_id;
}

wiced_result_t ble_app_start_adv(rt_bool_t high_duty)
{
    if (!ble_initialized)
        return WICED_BT_ERROR;

    ble_app_set_advertisement_data();

    return wiced_bt_start_advertisements(
        high_duty ? BTM_BLE_ADVERT_UNDIRECTED_HIGH : BTM_BLE_ADVERT_UNDIRECTED_LOW,
        0, NULL);
}

wiced_result_t ble_app_stop_adv(void)
{
    if (!ble_initialized)
        return WICED_BT_ERROR;

    return wiced_bt_start_advertisements(BTM_BLE_ADVERT_OFF, 0, NULL);
}

wiced_result_t ble_app_start_scan(wiced_bt_ble_scan_result_cback_t *callback)
{
    if (!ble_initialized)
        return WICED_BT_ERROR;

    return wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_HIGH_DUTY, WICED_TRUE, callback);
}

wiced_result_t ble_app_stop_scan(void)
{
    if (!ble_initialized)
        return WICED_BT_ERROR;

    return wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_NONE, WICED_TRUE, NULL);
}

wiced_bt_gatt_status_t ble_app_send_notification(const uint8_t *data, uint16_t len)
{
    if (!ble_initialized || !ble_state.is_connected)
        return WICED_BT_GATT_WRONG_STATE;

    if (!(ble_state.cccd_flags & GATT_CLIENT_CONFIG_NOTIFICATION))
        return WICED_BT_GATT_WRONG_STATE;

    if (len > BLE_APP_MAX_DATA_LEN)
        len = BLE_APP_MAX_DATA_LEN;

    /* Update TX buffer */
    memcpy(tx_data_buffer, data, len);
    tx_data_len = len;

    return wiced_bt_gatt_server_send_notification(ble_state.conn_id,
                                                   HANDLE_BLE_DATA_CHAR_TX_VAL,
                                                   len, tx_data_buffer, NULL);
}

void ble_app_set_rx_callback(ble_app_rx_callback_t callback)
{
    rx_callback = callback;
}

wiced_result_t ble_app_disconnect(void)
{
    if (!ble_initialized || !ble_state.is_connected)
        return WICED_BT_ERROR;

    /* wiced_bt_gatt_disconnect returns wiced_bt_gatt_status_t, cast to wiced_result_t */
    return (wiced_result_t)wiced_bt_gatt_disconnect(ble_state.conn_id);
}

void ble_app_get_local_addr(wiced_bt_device_address_t addr)
{
    wiced_bt_dev_read_local_addr(addr);
}

const char* ble_app_get_device_name(void)
{
    return (const char *)ble_app_cfg_settings.device_name;
}

const ble_app_state_t* ble_app_get_state(void)
{
    return &ble_state;
}
