/***********************************************************************************************//**
 * \file cyabs_rtos_impl.h
 *
 * \brief
 * Internal definitions for RTOS abstraction layer for RT-Thread
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
#include "cyabs_rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************
*                 Constants
******************************************************/

/** Minimum stack size in bytes */
#define CY_RTOS_MIN_STACK_SIZE      512

/** Minimum alignment for RTOS objects */
#define CY_RTOS_ALIGNMENT           RT_ALIGN_SIZE

/** Mask for checking the alignment of created RTOS objects */
#define CY_RTOS_ALIGNMENT_MASK      (RT_ALIGN_SIZE - 1)

/** Never timeout value - matches CY_RTOS_NEVER_TIMEOUT in cyabs_rtos.h */
#ifndef CY_RTOS_NEVER_TIMEOUT
#define CY_RTOS_NEVER_TIMEOUT       ( (uint32_t)0xffffffffUL )
#endif

/******************************************************
*                 Function Declarations
******************************************************/

/**
 * \brief Convert milliseconds to RT-Thread ticks
 *
 * @param[in] timeout_ms  Timeout in milliseconds
 * @return Timeout in RT-Thread ticks
 */
static inline rt_int32_t convert_ms_to_ticks(cy_time_t timeout_ms)
{
    if (timeout_ms == CY_RTOS_NEVER_TIMEOUT)
    {
        return RT_WAITING_FOREVER;
    }
    else if (timeout_ms == 0)
    {
        return 0;
    }
    else
    {
        return rt_tick_from_millisecond(timeout_ms);
    }
}

/**
 * \brief Check if code is running in ISR context
 * @return true if in ISR, false otherwise
 */
static inline bool is_in_isr(void)
{
    return (rt_interrupt_get_nest() > 0);
}

/******************************************************
*  FreeRTOS Compatibility Macros
*
*  The WPA3 external supplicant (Infineon original code)
*  uses taskENTER_CRITICAL / taskEXIT_CRITICAL directly.
*
*  In FreeRTOS, taskENTER_CRITICAL only masks interrupts
*  but the scheduler can still run inside kernel APIs
*  like xSemaphoreTake.  RT-Thread's rt_enter_critical()
*  suspends the scheduler entirely, which causes assertion
*  failures when wpa3_supplicant_deinit_workspace() calls
*  cy_rtos_join_thread() (→ rt_sem_take) inside the
*  critical section.
*
*  The critical section in the WPA3 supplicant protects
*  workspace teardown from a concurrent WCM worker task.
*  Since our RT-Thread porting uses a lightweight WCM shim
*  with no worker task, no actual protection is needed.
*  Map to no-ops to avoid the deadlock.
******************************************************/
#ifndef taskENTER_CRITICAL
#define taskENTER_CRITICAL()    do { } while(0)
#endif
#ifndef taskEXIT_CRITICAL
#define taskEXIT_CRITICAL()     do { } while(0)
#endif

#ifdef __cplusplus
} // extern "C"
#endif
