/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: www.aerolynq.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 *   Optional BLE L2CAP LE CoC (Credit Based Connection-Oriented Channel) demo.
 *   This module is intentionally feature-gated to keep the default BLE demo simple.
 */

#ifndef BLE_COC_H
#define BLE_COC_H

#include <rtthread.h>
#include <stdint.h>

#ifdef WHD_USING_BLUETOOTH

#ifndef BLE_APP_ENABLE_COC
#define BLE_APP_ENABLE_COC 1
#endif

#if BLE_APP_ENABLE_COC

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    rt_bool_t inited;
    rt_bool_t connected;
    uint16_t psm;
    uint16_t local_cid;
    uint16_t our_mtu;
    uint16_t peer_mtu;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    rt_tick_t start_tick;
    rt_tick_t end_tick;
} ble_coc_state_t;

rt_err_t ble_coc_init(uint16_t psm, uint16_t mtu);
rt_err_t ble_coc_connect(const uint8_t bd_addr[6], uint8_t addr_type);
rt_err_t ble_coc_disconnect(void);
rt_err_t ble_coc_send(const uint8_t *data, uint16_t len);
rt_err_t ble_coc_flood_start(uint16_t payload_len);
rt_err_t ble_coc_flood_stop(void);
const ble_coc_state_t *ble_coc_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_APP_ENABLE_COC */
#endif /* WHD_USING_BLUETOOTH */
#endif /* BLE_COC_H */
