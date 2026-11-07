/***********************************************************************************************//**
 * \file cyabs_rtos_dsram.h
 *
 * \brief
 * Internal definitions for DSRAM implementation on RT-Thread
 * Note: DSRAM (Deep Sleep RAM) functionality may not be directly supported
 * on all RT-Thread platforms. This is a placeholder for future implementation.
 *
 ***************************************************************************************************
 * \copyright
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 * Website: https://www.aerolynq.com
 *
 * SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
 *
 * PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
 * Use of this file is permitted EXCLUSIVELY with Aerolynq WiFi /
 * Bluetooth combo modules.  See the NOTICE and LICENSE files at the
 * root of this package for the complete terms.
 **************************************************************************************************/

#pragma once

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************
*                 Function Declarations
******************************************************/

/** Exits DS-RAM Low Power Mode.
 *
 * Note: This is a placeholder. Actual implementation depends on
 * the specific hardware platform and RT-Thread power management support.
 */
void cyabs_rtos_exit_dsram(void);

/** Stores DS-RAM Context.
 *
 * Note: This is a placeholder. Actual implementation depends on
 * the specific hardware platform and RT-Thread power management support.
 */
void vStoreDSRAMContextWithWFI(void);

/** Restores DS-RAM Context.
 *
 * Note: This is a placeholder. Actual implementation depends on
 * the specific hardware platform and RT-Thread power management support.
 */
void vRestoreDSRAMContext(void);

#ifdef __cplusplus
} // extern "C"
#endif
