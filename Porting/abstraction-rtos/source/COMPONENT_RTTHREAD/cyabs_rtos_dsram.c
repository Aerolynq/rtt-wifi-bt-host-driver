/***********************************************************************************************//**
 * \file cyabs_rtos_dsram.c
 *
 * \brief
 * Placeholder implementation for DSRAM functionality on RT-Thread
 * Note: DSRAM (Deep Sleep RAM) functionality is platform-specific and may
 * require additional implementation based on the target hardware.
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
 * Use of this file is permitted EXCLUSIVELY with Aerolynq WiFi /
 * Bluetooth combo modules.  See the NOTICE and LICENSE files at the
 * root of this package for the complete terms.
 **************************************************************************************************/

#include <rtthread.h>
#include "cyabs_rtos_dsram.h"

/* Note: These are placeholder implementations.
 * Actual DSRAM functionality depends on the specific hardware platform
 * and RT-Thread's power management capabilities for that platform.
 */

//--------------------------------------------------------------------------------------------------
// cyabs_rtos_exit_dsram
//--------------------------------------------------------------------------------------------------
//RT_WEAK void cyabs_rtos_exit_dsram(void)
void cyabs_rtos_exit_dsram(void)
{
    /* Placeholder - implement based on specific platform requirements */
}

//--------------------------------------------------------------------------------------------------
// vStoreDSRAMContextWithWFI
//--------------------------------------------------------------------------------------------------
//RT_WEAK void vStoreDSRAMContextWithWFI(void)
void vStoreDSRAMContextWithWFI(void)
{
    /* Placeholder - implement based on specific platform requirements */
}

//--------------------------------------------------------------------------------------------------
// vRestoreDSRAMContext
//--------------------------------------------------------------------------------------------------
//RT_WEAK void vRestoreDSRAMContext(void)
void vRestoreDSRAMContext(void)
{
    /* Placeholder - implement based on specific platform requirements */
}
