/***********************************************************************************************//**
 * \file whd_wlan_low_power.h
 *
 * \brief
 * Internal header shared between aerolynq_bt_wlan.c and whd_low_power.c.
 * Provides accessor functions for the low-power offload MSH commands
 * (wifi_ps, wifi_arp, wifi_tko, wifi_pf, wifi_wowl) to access the
 * STA interface and WLAN device without exposing internal structures.
 *
 ***************************************************************************************************
 * \copyright
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 *                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
 * Website: https://www.aerolynq.com
 *
 * SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
 *
 * PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
 * Use is permitted EXCLUSIVELY with Aerolynq WiFi / Bluetooth combo
 * modules.  See the NOTICE and LICENSE files at the root of this
 * package for the complete terms.
 **************************************************************************************************/

#ifndef WHD_WLAN_LOW_POWER_H
#define WHD_WLAN_LOW_POWER_H

#include <rtthread.h>
/* WLAN header compatibility for different RT-Thread versions */
#if (RT_VER_NUM >= 0x50200)
#include <drivers/wlan.h>
#else
#include <wlan_dev.h>
#endif
#include "whd.h"
#include "whd_wifi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the STA WHD interface handle
 * @return WHD interface pointer, or NULL if STA is not initialized
 *
 * Defined in aerolynq_bt_wlan.c
 */
whd_interface_t whd_lp_get_sta_itf(void);

/**
 * @brief Get the STA rt_wlan_device handle
 * @return rt_wlan_device pointer, or NULL if STA is not initialized
 *
 * Defined in aerolynq_bt_wlan.c
 */
struct rt_wlan_device *whd_lp_get_sta_wlan(void);

/**
 * @brief Set powersave mode on a WLAN device
 * @param wlan  WLAN device handle
 * @param level 0 = off, 1 = PM1, 2 = PM2
 * @return RT_EOK on success
 *
 * Defined in aerolynq_bt_wlan.c
 */
rt_err_t whd_lp_set_powersave(struct rt_wlan_device *wlan, int level);

#ifdef __cplusplus
}
#endif

#endif /* WHD_WLAN_LOW_POWER_H */
