/***********************************************************************************************//**
 * \file cyabs_rtos_internal.h
 *
 * \brief
 * Internal interface used for RTOS abstraction utilities.
 ***************************************************************************************************
 * \copyright
 * Copyright 2018-2022 Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **************************************************************************************************/

#pragma once

/* Modified by Aerolynq (Xinlian Xinwei (Hangzhou) Technology Co., Ltd.), 2025-2026,
 * for the RT-Thread port. The is_in_isr() implementation now takes the
 * RT-Thread and Zephyr branches into account.
 * The rest of this file is unchanged Infineon code under Apache-2.0. */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Platform-specific ISR check implementation */
#if defined(RT_USING_HEAP) || defined(CONFIG_ZEPHYR)
/* RT-Thread/Zephyr: is_in_isr() is defined in cyabs_rtos_impl.h, skip here */

#elif defined(COMPONENT_CAT5)
/* CAT5 device does not allow calling from interrupt context */
static inline bool is_in_isr(void)
{
    return false;
}

#else
/* CMSIS-based implementation for Cortex-M/R/A */
#include <cmsis_compiler.h>

static inline bool is_in_isr(void)
{
    #if defined(COMPONENT_CR4) // Cortex-A & Cortex-R
    uint32_t mode = __get_mode();
    return (mode == 0x11U /*FIQ*/) || (mode == 0x12U /*IRQ*/) || (mode == 0x13U /*SVC*/) ||
           (mode == 0x17U /*ABT*/) || (mode == 0x1BU /*UND*/);
    #else // Cortex-M
    return (__get_IPSR() != 0);
    #endif
}
#endif


#if defined(__cplusplus)
}
#endif
