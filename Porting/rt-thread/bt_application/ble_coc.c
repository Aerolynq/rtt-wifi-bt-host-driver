/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 *   Optional BLE L2CAP LE CoC demo implementation.
 */

#include <rtthread.h>
#include <string.h>

#ifdef WHD_USING_BLUETOOTH

#include "ble_coc.h"

#if BLE_APP_ENABLE_COC

#include "wiced_bt_l2c.h"
#include "wiced_bt_ble.h"
#include "wiced_bt_types.h"
#include "wiced_bt_stack.h"
#include "wiced_memory.h"

#define DBG_TAG "ble.coc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifndef BLE_COC_DEFAULT_PSM
#define BLE_COC_DEFAULT_PSM 19
#endif

#ifndef BLE_COC_DEFAULT_MTU
#define BLE_COC_DEFAULT_MTU 100
#endif

#ifndef BLE_COC_MAX_MTU
#define BLE_COC_MAX_MTU 512
#endif

#define BLE_COC_INVALID_CID 0xFFFF

static ble_coc_state_t g_coc;

static struct
{
    rt_thread_t tx_thread;
    struct rt_semaphore tx_sem;
    rt_bool_t tx_thread_started;
    rt_bool_t flood_enabled;
    uint16_t flood_payload_len;
    uint8_t flood_buf[BLE_COC_MAX_MTU];
} g_tx;

static void coc_release_drb(tDRB *p_drb)
{
    if (p_drb)
        wiced_bt_free_buffer(p_drb);
}

static void coc_connect_ind(wiced_bt_device_address_t bda, uint16_t local_cid, uint16_t psm, uint8_t id, uint16_t mtu_peer)
{
    tDRB *p_rx_drb;

    LOG_I("CoC connect_ind: cid=%u psm=%u peer_mtu=%u", local_cid, psm, mtu_peer);

    if (!g_coc.inited)
        return;

    if (g_coc.local_cid != BLE_COC_INVALID_CID)
    {
        LOG_W("CoC already connected (cid=%u), reject", g_coc.local_cid);
        wiced_bt_l2cap_le_connect_rsp(bda, id, local_cid, L2CAP_CONN_NO_RESOURCES, g_coc.our_mtu, NULL);
        return;
    }

    p_rx_drb = (tDRB *)wiced_bt_get_buffer(g_coc.our_mtu + DRB_OVERHEAD_SIZE);
    if (!p_rx_drb)
    {
        LOG_E("CoC alloc RX DRB failed");
        wiced_bt_l2cap_le_connect_rsp(bda, id, local_cid, L2CAP_CONN_NO_RESOURCES, g_coc.our_mtu, NULL);
        return;
    }
    memset(p_rx_drb, 0, g_coc.our_mtu + DRB_OVERHEAD_SIZE);

    /* Accept connection */
    wiced_bt_l2cap_le_connect_rsp(bda, id, local_cid, L2CAP_CONN_OK, g_coc.our_mtu, p_rx_drb);

    /* Stop advertising (same as reference implementation) */
    wiced_bt_start_advertisements(BTM_BLE_ADVERT_OFF, 0, NULL);

    g_coc.connected = RT_TRUE;
    g_coc.local_cid = local_cid;
    g_coc.peer_mtu = mtu_peer;
    g_coc.tx_bytes = 0;
    g_coc.rx_bytes = 0;
    g_coc.start_tick = rt_tick_get();
    g_coc.end_tick = 0;
}

static void coc_connect_cfm(uint16_t local_cid, uint16_t result, uint16_t mtu_peer)
{
    LOG_I("CoC connect_cfm: cid=%u result=%u peer_mtu=%u", local_cid, result, mtu_peer);

    if (!g_coc.inited)
        return;

    if (result == 0)
    {
        g_coc.connected = RT_TRUE;
        g_coc.local_cid = local_cid;
        g_coc.peer_mtu = mtu_peer;
        g_coc.tx_bytes = 0;
        g_coc.rx_bytes = 0;
        g_coc.start_tick = rt_tick_get();
        g_coc.end_tick = 0;
    }
}

static void coc_disconnect_ind(uint16_t local_cid, uint16_t reason, wiced_bool_t ack)
{
    LOG_W("CoC disconnect_ind: cid=%u reason=%u", local_cid, reason);

    if (ack)
        wiced_bt_l2cap_le_disconnect_rsp(local_cid);

    if (g_coc.local_cid == local_cid)
    {
        g_coc.connected = RT_FALSE;
        g_coc.local_cid = BLE_COC_INVALID_CID;
        g_coc.end_tick = rt_tick_get();
        g_tx.flood_enabled = RT_FALSE;
    }
}

static void coc_disconnect_cfm(uint16_t local_cid, uint16_t result)
{
    LOG_I("CoC disconnect_cfm: cid=%u result=%u", local_cid, result);

    if (g_coc.local_cid == local_cid)
    {
        g_coc.connected = RT_FALSE;
        g_coc.local_cid = BLE_COC_INVALID_CID;
        g_coc.end_tick = rt_tick_get();
        g_tx.flood_enabled = RT_FALSE;
    }
}

static void coc_data_ind(uint16_t local_cid, tDRB *p_drb)
{
    uint16_t len;

    if (!p_drb)
        return;

    len = p_drb->drb_data_len;

    if (!g_coc.connected || g_coc.local_cid != local_cid)
        return;

    if (g_coc.rx_bytes == 0)
        g_coc.start_tick = rt_tick_get();

    g_coc.rx_bytes += len;
}

static void coc_tx_complete(uint16_t local_cid, void *p_data)
{
    RT_UNUSED(p_data);

    if (!g_coc.connected || g_coc.local_cid != local_cid)
        return;

    /* Allow next flood packet */
    rt_sem_release(&g_tx.tx_sem);
}

static wiced_bt_l2cap_le_appl_information_t g_l2c_appl_info = {
    .le_connect_indication_cback = coc_connect_ind,
    .le_connect_confirm_cback = coc_connect_cfm,
    .disconnect_indication_cback = coc_disconnect_ind,
    .disconnect_confirm_cback = coc_disconnect_cfm,
    .data_indication_cback = coc_data_ind,
    .le_tx_complete_cback = coc_tx_complete,
    .le_release_drb_cb = coc_release_drb,
};

static void coc_tx_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        /* Wait until previous TX completes (or flood is started) */
        rt_sem_take(&g_tx.tx_sem, RT_WAITING_FOREVER);

        if (!g_tx.flood_enabled)
            continue;

        if (!g_coc.connected || g_coc.local_cid == BLE_COC_INVALID_CID)
            continue;

        if (g_tx.flood_payload_len == 0)
            continue;

        {
            uint8_t ret = wiced_bt_l2cap_le_data_write(g_coc.local_cid, g_tx.flood_buf, g_tx.flood_payload_len);
            if (ret == L2CAP_DATAWRITE_SUCCESS || ret == L2CAP_DATAWRITE_CONGESTED)
            {
                /* Both SUCCESS and CONGESTED mean data was accepted */
                g_coc.tx_bytes += g_tx.flood_payload_len;
                /* tx_complete callback will release semaphore for next packet */
            }
            else
            {
                /* FAILED - back off a bit then retry */
                rt_thread_mdelay(10);
                rt_sem_release(&g_tx.tx_sem);
            }
        }
    }
}

static rt_err_t ensure_tx_thread(void)
{
    if (g_tx.tx_thread_started)
        return RT_EOK;

    if (rt_sem_init(&g_tx.tx_sem, "coc_tx", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
        return -RT_ERROR;

    g_tx.tx_thread = rt_thread_create("coc_tx", coc_tx_thread_entry, NULL, 2048, RT_THREAD_PRIORITY_MAX - 2, 10);
    if (!g_tx.tx_thread)
        return -RT_ERROR;

    rt_thread_startup(g_tx.tx_thread);
    g_tx.tx_thread_started = RT_TRUE;
    return RT_EOK;
}

rt_err_t ble_coc_init(uint16_t psm, uint16_t mtu)
{
    uint16_t reg_psm;

    if (psm == 0)
        psm = BLE_COC_DEFAULT_PSM;

    if (mtu == 0)
        mtu = BLE_COC_DEFAULT_MTU;

    if (mtu > BLE_COC_MAX_MTU)
        mtu = BLE_COC_MAX_MTU;

    memset(&g_coc, 0, sizeof(g_coc));
    g_coc.psm = psm;
    g_coc.our_mtu = mtu;
    g_coc.peer_mtu = 0;
    g_coc.local_cid = BLE_COC_INVALID_CID;

    reg_psm = wiced_bt_l2cap_le_register(psm, &g_l2c_appl_info);
    if (reg_psm == 0)
    {
        LOG_E("CoC register failed (psm=%u)", psm);
        return -RT_ERROR;
    }

    if (ensure_tx_thread() != RT_EOK)
    {
        LOG_E("CoC tx thread init failed");
        return -RT_ERROR;
    }

    /* Prepare flood buffer */
    for (uint16_t i = 0; i < sizeof(g_tx.flood_buf); i++)
        g_tx.flood_buf[i] = 'A';

    g_coc.inited = RT_TRUE;
    LOG_I("CoC inited: psm=%u mtu=%u", g_coc.psm, g_coc.our_mtu);
    return RT_EOK;
}

rt_err_t ble_coc_connect(const uint8_t bd_addr[6], uint8_t addr_type)
{
    tDRB *p_rx_drb;
    wiced_bt_device_address_t addr;

    if (!g_coc.inited)
        return -RT_ERROR;

    if (g_coc.connected)
        return -RT_EBUSY;

    p_rx_drb = (tDRB *)wiced_bt_get_buffer(g_coc.our_mtu + DRB_OVERHEAD_SIZE);
    if (!p_rx_drb)
        return -RT_ERROR;

    memset(p_rx_drb, 0, g_coc.our_mtu + DRB_OVERHEAD_SIZE);

    memcpy(addr, bd_addr, sizeof(addr));

    if (wiced_bt_l2cap_le_connect_req(g_coc.psm, addr, (wiced_bt_ble_address_type_t)addr_type,
                                     BLE_CONN_MODE_HIGH_DUTY, g_coc.our_mtu, 0, 0, p_rx_drb) == 0)
    {
        wiced_bt_free_buffer(p_rx_drb);
        return -RT_ERROR;
    }

    LOG_I("CoC connect requested");
    return RT_EOK;
}

rt_err_t ble_coc_disconnect(void)
{
    if (!g_coc.connected || g_coc.local_cid == BLE_COC_INVALID_CID)
        return -RT_ERROR;

    wiced_bt_l2cap_le_disconnect_req(g_coc.local_cid);
    return RT_EOK;
}

rt_err_t ble_coc_send(const uint8_t *data, uint16_t len)
{
    if (!g_coc.connected || g_coc.local_cid == BLE_COC_INVALID_CID)
        return -RT_ERROR;

    if (len == 0)
        return RT_EOK;

    if (g_coc.peer_mtu && len > g_coc.peer_mtu)
        len = g_coc.peer_mtu;

    {
        uint8_t ret = wiced_bt_l2cap_le_data_write(g_coc.local_cid, (uint8_t *)data, len);
        if (ret == L2CAP_DATAWRITE_SUCCESS || ret == L2CAP_DATAWRITE_CONGESTED)
        {
            g_coc.tx_bytes += len;
            return RT_EOK;
        }
    }

    return -RT_ERROR;
}

rt_err_t ble_coc_flood_start(uint16_t payload_len)
{
    if (!g_coc.connected || g_coc.local_cid == BLE_COC_INVALID_CID)
        return -RT_ERROR;

    if (payload_len == 0)
        payload_len = g_coc.our_mtu;

    if (payload_len > BLE_COC_MAX_MTU)
        payload_len = BLE_COC_MAX_MTU;

    g_tx.flood_payload_len = payload_len;
    g_tx.flood_enabled = RT_TRUE;

    /* Kick the first packet */
    rt_sem_release(&g_tx.tx_sem);
    return RT_EOK;
}

rt_err_t ble_coc_flood_stop(void)
{
    g_tx.flood_enabled = RT_FALSE;
    return RT_EOK;
}

const ble_coc_state_t *ble_coc_get_state(void)
{
    return &g_coc;
}

#endif /* BLE_APP_ENABLE_COC */
#endif /* WHD_USING_BLUETOOTH */
