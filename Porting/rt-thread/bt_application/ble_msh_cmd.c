/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-24     Aerolynq     Initial version - BLE MSH commands
 *
 * Description:
 *   MSH shell commands for BLE control and debugging.
 *   Includes commands from bluetooth_utility adapted for RT-Thread.
 */

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef WHD_USING_BLUETOOTH

#include "ble_app.h"
#include "ble_coc.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_dev.h"

#define DBG_TAG    "ble.msh"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* Set to 1 if you want the legacy flat commands (ble_start/ble_send/...) in addition
 * to the recommended unified command: 'ble <subcommand>'.
 */
#ifndef BLE_MSH_ENABLE_LEGACY_CMDS
#define BLE_MSH_ENABLE_LEGACY_CMDS 0
#endif

/*******************************************************************************
 * Static Variables
 ******************************************************************************/
static rt_bool_t ble_scan_active = RT_FALSE;

#ifndef BLE_APP_ENABLE_COC
#define BLE_APP_ENABLE_COC 0
#endif

#if BLE_APP_ENABLE_COC
static int ble_msh_parse_bdaddr(const char *s, uint8_t out[6])
{
    unsigned int b[6];

    if (!s || !out)
        return -1;

    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return -1;

    for (int i = 0; i < 6; i++)
        out[i] = (uint8_t)b[i];

    return 0;
}

static int cmd_ble_coc(int argc, char **argv)
{
    const ble_coc_state_t *st;

    if (argc < 2)
        goto show_help;

    if (rt_strcmp(argv[1], "init") == 0)
    {
        uint16_t psm = 19;
        uint16_t mtu = 100;

        if (argc >= 3)
            psm = (uint16_t)atoi(argv[2]);
        if (argc >= 4)
            mtu = (uint16_t)atoi(argv[3]);

        if (ble_coc_init(psm, mtu) == RT_EOK)
        {
            rt_kprintf("CoC initialized: psm=%d mtu=%d\n", psm, mtu);
            return 0;
        }
        rt_kprintf("CoC init failed\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "connect") == 0)
    {
        uint8_t addr[6];
        uint8_t addr_type = BLE_ADDR_PUBLIC;

        if (argc < 3)
        {
            rt_kprintf("Usage: ble coc connect <aa:bb:cc:dd:ee:ff> [public|random]\n");
            return -1;
        }

        if (ble_msh_parse_bdaddr(argv[2], addr) != 0)
        {
            rt_kprintf("Invalid address format\n");
            return -1;
        }

        if (argc >= 4)
        {
            if (rt_strcmp(argv[3], "random") == 0)
                addr_type = BLE_ADDR_RANDOM;
            else
                addr_type = BLE_ADDR_PUBLIC;
        }

        if (ble_coc_connect(addr, addr_type) == RT_EOK)
        {
            rt_kprintf("CoC connect requested\n");
            return 0;
        }
        rt_kprintf("CoC connect request failed\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "disconnect") == 0)
    {
        if (ble_coc_disconnect() == RT_EOK)
        {
            rt_kprintf("CoC disconnect requested\n");
            return 0;
        }
        rt_kprintf("CoC disconnect failed\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "send") == 0)
    {
        char data[256];
        int len = 0;

        if (argc < 3)
        {
            rt_kprintf("Usage: ble coc send <data>\n");
            return -1;
        }

        for (int i = 2; i < argc && len < (int)sizeof(data) - 1; i++)
        {
            int arg_len = rt_strlen(argv[i]);
            if (len + arg_len + 1 > (int)sizeof(data) - 1)
                arg_len = (int)sizeof(data) - 1 - len - 1;

            if (i > 2 && len < (int)sizeof(data) - 1)
                data[len++] = ' ';

            memcpy(&data[len], argv[i], arg_len);
            len += arg_len;
        }
        data[len] = '\0';

        if (ble_coc_send((const uint8_t *)data, (uint16_t)len) == RT_EOK)
        {
            rt_kprintf("CoC sent %d bytes\n", len);
            return 0;
        }
        rt_kprintf("CoC send failed\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "flood") == 0)
    {
        if (argc >= 3 && rt_strcmp(argv[2], "start") == 0)
        {
            uint16_t payload_len = 0;
            if (argc >= 4)
                payload_len = (uint16_t)atoi(argv[3]);

            if (ble_coc_flood_start(payload_len) == RT_EOK)
            {
                rt_kprintf("CoC flood started (len=%d)\n", payload_len ? payload_len : 0);
                return 0;
            }
            rt_kprintf("CoC flood start failed\n");
            return -1;
        }
        else if (argc >= 3 && rt_strcmp(argv[2], "stop") == 0)
        {
            ble_coc_flood_stop();
            rt_kprintf("CoC flood stopped\n");
            return 0;
        }

        rt_kprintf("Usage: ble coc flood <start [len]|stop>\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "stat") == 0 || rt_strcmp(argv[1], "info") == 0)
    {
        st = ble_coc_get_state();
        if (!st)
            return -1;

        rt_kprintf("=== CoC Status ===\n");
        rt_kprintf("  Inited:    %s\n", st->inited ? "Yes" : "No");
        rt_kprintf("  Connected: %s\n", st->connected ? "Yes" : "No");
        rt_kprintf("  PSM:       %d\n", st->psm);
        rt_kprintf("  CID:       %d\n", st->local_cid);
        rt_kprintf("  Our MTU:   %d\n", st->our_mtu);
        rt_kprintf("  Peer MTU:  %d\n", st->peer_mtu);
        rt_kprintf("  TX bytes:  %u\n", (unsigned int)st->tx_bytes);
        rt_kprintf("  RX bytes:  %u\n", (unsigned int)st->rx_bytes);
        return 0;
    }

show_help:
    rt_kprintf("CoC Commands:\n");
    rt_kprintf("  ble coc init [psm] [mtu]                 - Register LE CoC (default psm=19 mtu=100)\n");
    rt_kprintf("  ble coc connect <addr> [public|random]   - Initiate CoC connection (central role)\n");
    rt_kprintf("  ble coc disconnect                       - Disconnect CoC channel\n");
    rt_kprintf("  ble coc send <data>                      - Send one CoC packet\n");
    rt_kprintf("  ble coc flood start [len]                - Continuous TX (throughput test)\n");
    rt_kprintf("  ble coc flood stop                       - Stop continuous TX\n");
    rt_kprintf("  ble coc stat                             - Show CoC status\n");
    return 0;
}
#endif /* BLE_APP_ENABLE_COC */

/*******************************************************************************
 * Scan Callback
 ******************************************************************************/

static void ble_msh_scan_callback(wiced_bt_ble_scan_results_t *p_scan_result, uint8_t *p_adv_data)
{
    if (p_scan_result)
    {
        uint8_t *p_data;
        uint8_t length;
        char name[32] = {0};
        
        /* Try to get device name from advertisement data */
        p_data = wiced_bt_ble_check_advertising_data(p_adv_data, BTM_BLE_ADVERT_TYPE_NAME_COMPLETE, &length);
        if (p_data == NULL)
        {
            p_data = wiced_bt_ble_check_advertising_data(p_adv_data, BTM_BLE_ADVERT_TYPE_NAME_SHORT, &length);
        }
        
        if (p_data && length > 0)
        {
            if (length > sizeof(name) - 1)
                length = sizeof(name) - 1;
            memcpy(name, p_data, length);
            name[length] = '\0';
        }
        else
        {
            rt_strcpy(name, "(unknown)");
        }
        
        rt_kprintf("  %02X:%02X:%02X:%02X:%02X:%02X  RSSI:%4d  %s\n",
                   p_scan_result->remote_bd_addr[0], p_scan_result->remote_bd_addr[1],
                   p_scan_result->remote_bd_addr[2], p_scan_result->remote_bd_addr[3],
                   p_scan_result->remote_bd_addr[4], p_scan_result->remote_bd_addr[5],
                   p_scan_result->rssi, name);
    }
    else
    {
        rt_kprintf("Scan completed.\n");
        ble_scan_active = RT_FALSE;
    }
}

/*******************************************************************************
 * RX Data Callback
 ******************************************************************************/

static void ble_msh_rx_callback(const uint8_t *data, uint16_t len)
{
    rt_kprintf("BLE RX [%d bytes]: ", len);
    for (uint16_t i = 0; i < len && i < 64; i++)
    {
        if (data[i] >= 0x20 && data[i] < 0x7F)
            rt_kprintf("%c", data[i]);
        else
            rt_kprintf(".");
    }
    if (len > 64)
        rt_kprintf("...");
    rt_kprintf("\n");
}

/*
 * Note on naming:
 * - BT stack init is done during boot/platform init.
 * - This command only initializes the GATT server database.
 *
 * User-facing command is: `ble gatt start`
 * `ble start` is kept as a backward-compatible alias.
 */

static int cmd_ble_start(int argc, char **argv)
{
    rt_err_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BT stack not initialized\n");
        return -1;
    }
    
    result = ble_app_gatt_init();
    
    if (result == RT_EOK)
    {
        rt_kprintf("BLE GATT initialized successfully.\n");
        rt_kprintf("Use 'ble adv start' to begin advertising.\n");
    }
    else
    {
        rt_kprintf("Failed to initialize GATT\n");
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_start, ble_start, Initialize BLE GATT service);
#endif

/*******************************************************************************
 * MSH Command: ble_info
 ******************************************************************************/

static int cmd_ble_info(int argc, char **argv)
{
    wiced_bt_device_address_t bda;
    const ble_app_state_t *state;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BT stack not initialized\n");
        return -1;
    }
    
    state = ble_app_get_state();
    ble_app_get_local_addr(bda);
    
    rt_kprintf("=== BLE Status ===\n");
    rt_kprintf("  BT Stack:      Initialized\n");
    rt_kprintf("  GATT:          %s\n", ble_app_is_gatt_initialized() ? "Initialized" : "Not initialized (use 'ble gatt start')");
    rt_kprintf("  Device Name:   %s\n", ble_app_get_device_name());
    rt_kprintf("  Local Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
               bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    rt_kprintf("  Connected:     %s\n", state->is_connected ? "Yes" : "No");
    
    if (state->is_connected)
    {
        rt_kprintf("  Peer Address:  %02X:%02X:%02X:%02X:%02X:%02X\n",
                   state->peer_addr[0], state->peer_addr[1], state->peer_addr[2],
                   state->peer_addr[3], state->peer_addr[4], state->peer_addr[5]);
        rt_kprintf("  Connection ID: %d\n", state->conn_id);
        rt_kprintf("  Peer MTU:      %d\n", state->peer_mtu);
        rt_kprintf("  Notify:        %s\n", 
                   (state->cccd_flags & GATT_CLIENT_CONFIG_NOTIFICATION) ? "Enabled" : "Disabled");
    }
    
    rt_kprintf("  Battery Level: %d%%\n", state->battery_level);
    rt_kprintf("  Scan Active:   %s\n", ble_scan_active ? "Yes" : "No");
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_info, ble_info, Show BLE status and device information);
#endif

/*******************************************************************************
 * MSH Command: ble_adv_start
 ******************************************************************************/

static int cmd_ble_adv_start(int argc, char **argv)
{
    wiced_result_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BT stack not initialized\n");
        return -1;
    }
    
    if (!ble_app_is_gatt_initialized())
    {
        rt_kprintf("GATT not initialized. Run 'ble gatt start' first.\n");
        return -1;
    }
    
    /* Keep it simple: always start advertising in high duty mode.
     * If you want low duty, adjust intervals in ble_app_cfg.c.
     */
    result = ble_app_start_adv(RT_TRUE);
    
    if (result == WICED_BT_SUCCESS)
    {
        rt_kprintf("BLE advertisement started (high duty, device: %s)\n",
                   ble_app_get_device_name());
    }
    else
    {
        rt_kprintf("Failed to start advertisement: 0x%x\n", result);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_adv_start, ble_adv_start, Start BLE advertisement);
#endif

/*******************************************************************************
 * MSH Command: ble_adv_stop
 ******************************************************************************/

static int cmd_ble_adv_stop(int argc, char **argv)
{
    wiced_result_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    result = ble_app_stop_adv();
    
    if (result == WICED_BT_SUCCESS)
    {
        rt_kprintf("BLE advertisement stopped\n");
    }
    else
    {
        rt_kprintf("Failed to stop advertisement: 0x%x\n", result);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_adv_stop, ble_adv_stop, Stop BLE advertisement);
#endif

/*******************************************************************************
 * MSH Command: ble_scan_start
 ******************************************************************************/

static int cmd_ble_scan_start(int argc, char **argv)
{
    wiced_result_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    if (ble_scan_active)
    {
        rt_kprintf("BLE scan already active\n");
        return -1;
    }
    
    rt_kprintf("Starting BLE scan...\n");
    result = ble_app_start_scan(ble_msh_scan_callback);
    
    if (result == WICED_BT_SUCCESS || result == WICED_BT_PENDING)
    {
        ble_scan_active = RT_TRUE;
        rt_kprintf("BLE scan started. Devices found:\n");
    }
    else
    {
        rt_kprintf("Failed to start scan: 0x%x\n", result);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_scan_start, ble_scan_start, Start BLE scan);
#endif

/*******************************************************************************
 * MSH Command: ble_scan_stop
 ******************************************************************************/

static int cmd_ble_scan_stop(int argc, char **argv)
{
    wiced_result_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    result = ble_app_stop_scan();
    
    if (result == WICED_BT_SUCCESS)
    {
        ble_scan_active = RT_FALSE;
        rt_kprintf("BLE scan stopped\n");
    }
    else
    {
        rt_kprintf("Failed to stop scan: 0x%x\n", result);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_scan_stop, ble_scan_stop, Stop BLE scan);
#endif

/*******************************************************************************
 * MSH Command: ble_send
 ******************************************************************************/

static int cmd_ble_send(int argc, char **argv)
{
    wiced_bt_gatt_status_t status;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    if (!ble_app_is_connected())
    {
        rt_kprintf("No BLE connection\n");
        return -1;
    }
    
    if (argc < 2)
    {
        rt_kprintf("Usage: ble_send <data>\n");
        return -1;
    }
    
    /* Join all arguments as data */
    char data[BLE_APP_MAX_DATA_LEN];
    int len = 0;
    
    for (int i = 1; i < argc && len < BLE_APP_MAX_DATA_LEN - 1; i++)
    {
        int arg_len = rt_strlen(argv[i]);
        if (len + arg_len + 1 > BLE_APP_MAX_DATA_LEN - 1)
            arg_len = BLE_APP_MAX_DATA_LEN - 1 - len - 1;
        
        if (i > 1 && len < BLE_APP_MAX_DATA_LEN - 1)
        {
            data[len++] = ' ';
        }
        memcpy(&data[len], argv[i], arg_len);
        len += arg_len;
    }
    data[len] = '\0';
    
    status = ble_app_send_notification((uint8_t *)data, len);
    
    if (status == WICED_BT_GATT_SUCCESS)
    {
        rt_kprintf("Sent %d bytes: %s\n", len, data);
    }
    else if (status == WICED_BT_GATT_WRONG_STATE)
    {
        rt_kprintf("Notifications not enabled by client\n");
        return -1;
    }
    else
    {
        rt_kprintf("Failed to send: 0x%x\n", status);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_send, ble_send, Send BLE notification data);
#endif

/*******************************************************************************
 * MSH Command: ble_disconnect
 ******************************************************************************/

static int cmd_ble_disconnect(int argc, char **argv)
{
    wiced_result_t result;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    if (!ble_app_is_connected())
    {
        rt_kprintf("No BLE connection\n");
        return -1;
    }
    
    result = ble_app_disconnect();
    
    if (result == WICED_BT_SUCCESS)
    {
        rt_kprintf("Disconnect initiated\n");
    }
    else
    {
        rt_kprintf("Failed to disconnect: 0x%x\n", result);
        return -1;
    }
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_disconnect, ble_disconnect, Disconnect BLE connection);
#endif

/*******************************************************************************
 * MSH Command: ble_addr
 ******************************************************************************/

static int cmd_ble_addr(int argc, char **argv)
{
    wiced_bt_device_address_t bda;
    
    if (!ble_app_is_initialized())
    {
        rt_kprintf("BLE not initialized\n");
        return -1;
    }
    
    ble_app_get_local_addr(bda);
    rt_kprintf("Local BLE Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
               bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    
    return 0;
}
#if BLE_MSH_ENABLE_LEGACY_CMDS
MSH_CMD_EXPORT_ALIAS(cmd_ble_addr, ble_addr, Show local BLE address);
#endif

/*******************************************************************************
 * MSH Command: ble (main command with subcommands)
 ******************************************************************************/

static int cmd_ble(int argc, char **argv)
{
    if (argc < 2)
    {
        goto show_help;
    }
    
    /* Backward-compatible alias: `ble start` */
    if (rt_strcmp(argv[1], "start") == 0)
    {
        rt_kprintf("Note: 'ble start' is deprecated. Use 'ble gatt start'.\n");
        return cmd_ble_start(argc - 1, &argv[1]);
    }
    else if (rt_strcmp(argv[1], "gatt") == 0)
    {
        if (argc >= 3 && (rt_strcmp(argv[2], "start") == 0 || rt_strcmp(argv[2], "init") == 0))
        {
            return cmd_ble_start(argc - 2, &argv[2]);
        }
        rt_kprintf("Usage: ble gatt <start>\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "info") == 0)
    {
        return cmd_ble_info(argc - 1, &argv[1]);
    }
    else if (rt_strcmp(argv[1], "adv") == 0)
    {
        if (argc >= 3)
        {
            if (rt_strcmp(argv[2], "start") == 0 || rt_strcmp(argv[2], "on") == 0)
            {
                return cmd_ble_adv_start(argc - 2, &argv[2]);
            }
            else if (rt_strcmp(argv[2], "stop") == 0 || rt_strcmp(argv[2], "off") == 0)
            {
                return cmd_ble_adv_stop(0, NULL);
            }
        }
        rt_kprintf("Usage: ble adv <start|stop>\n");
        return -1;
    }
#if BLE_APP_ENABLE_COC
    else if (rt_strcmp(argv[1], "coc") == 0)
    {
        return cmd_ble_coc(argc - 1, &argv[1]);
    }
#endif
    else if (rt_strcmp(argv[1], "scan") == 0)
    {
        if (argc >= 3)
        {
            if (rt_strcmp(argv[2], "start") == 0 || rt_strcmp(argv[2], "on") == 0)
            {
                return cmd_ble_scan_start(0, NULL);
            }
            else if (rt_strcmp(argv[2], "stop") == 0 || rt_strcmp(argv[2], "off") == 0)
            {
                return cmd_ble_scan_stop(0, NULL);
            }
        }
        rt_kprintf("Usage: ble scan <start|stop>\n");
        return -1;
    }
    else if (rt_strcmp(argv[1], "send") == 0)
    {
        return cmd_ble_send(argc - 1, &argv[1]);
    }
    else if (rt_strcmp(argv[1], "disconnect") == 0 || rt_strcmp(argv[1], "disc") == 0)
    {
        return cmd_ble_disconnect(0, NULL);
    }
    else if (rt_strcmp(argv[1], "addr") == 0)
    {
        return cmd_ble_addr(0, NULL);
    }
    else if (rt_strcmp(argv[1], "help") == 0)
    {
        goto show_help;
    }
    else
    {
        rt_kprintf("Unknown command: %s\n", argv[1]);
        goto show_help;
    }

show_help:
    rt_kprintf("BLE Commands:\n");
    rt_kprintf("  ble gatt start        - Initialize GATT for BLE peripheral\n");
    rt_kprintf("  ble info              - Show BLE status and connection info\n");
    rt_kprintf("  ble addr              - Show local BLE address\n");
    rt_kprintf("  ble adv start         - Start advertisement (high duty)\n");
    rt_kprintf("  ble adv stop          - Stop advertisement\n");
#if BLE_APP_ENABLE_COC
    rt_kprintf("  ble coc ...           - LE CoC commands (type 'ble coc')\n");
#endif
    rt_kprintf("  ble scan start        - Start scanning for BLE devices\n");
    rt_kprintf("  ble scan stop         - Stop scanning\n");
    rt_kprintf("  ble send <data>       - Send notification to connected client\n");
    rt_kprintf("  ble disconnect        - Disconnect current connection\n");
    rt_kprintf("  ble help              - Show this help\n");
    rt_kprintf("\nUsage flow:\n");
    rt_kprintf("  1. ble gatt start     - Init GATT (required once)\n");
    rt_kprintf("  2. ble adv start      - Start advertising\n");
    rt_kprintf("  3. Connect from phone\n");
    rt_kprintf("  4. ble send <data>    - Send data to phone\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ble, ble, BLE control commands - type 'ble help' for usage);

/*******************************************************************************
 * Initialization - Register RX callback
 ******************************************************************************/

static int ble_msh_init(void)
{
    /* Register RX callback to print received data */
    ble_app_set_rx_callback(ble_msh_rx_callback);
    return 0;
}
INIT_APP_EXPORT(ble_msh_init);

#endif /* WHD_USING_BLUETOOTH */
