/***********************************************************************************************//**
 * \file cyabs_rtos_types.h
 *
 * \brief
 * Internal type definitions for RTOS abstraction layer for RT-Thread
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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************
*                   Enumerations
******************************************************/

/**
 * \brief Thread priority mapping for RT-Thread
 *
 * RT-Thread uses lower numbers for higher priority.
 * RT_THREAD_PRIORITY_MAX is the lowest priority (highest number).
 * Priority 0 is the highest.
 */
typedef enum cy_thread_priority
{
    CY_RTOS_PRIORITY_MIN         = (RT_THREAD_PRIORITY_MAX - 1),
    CY_RTOS_PRIORITY_LOW         = (RT_THREAD_PRIORITY_MAX - 2),
    CY_RTOS_PRIORITY_BELOWNORMAL = (RT_THREAD_PRIORITY_MAX - 3),
    CY_RTOS_PRIORITY_NORMAL      = (RT_THREAD_PRIORITY_MAX / 2),
    CY_RTOS_PRIORITY_ABOVENORMAL = (RT_THREAD_PRIORITY_MAX / 3),
    CY_RTOS_PRIORITY_HIGH        = (RT_THREAD_PRIORITY_MAX / 4),
    CY_RTOS_PRIORITY_REALTIME    = 2,
    CY_RTOS_PRIORITY_MAX         = 1
} cy_thread_priority_t;

/******************************************************
*                 Type Definitions
******************************************************/

/** Alias for the RTOS specific argument passed to the timer callback function */
typedef rt_ubase_t cy_timer_callback_arg_t;

/** Alias for the RTOS specific time unit (in milliseconds) */
typedef uint32_t cy_time_t;

/** Alias for the RTOS specific argument passed to the entry function of a thread */
typedef void* cy_thread_arg_t;

/** Alias for the RTOS specific definition of an error status */
typedef rt_err_t cy_rtos_error_t;

/**
 * \brief Mutex wrapper structure for RT-Thread
 */
typedef struct
{
    rt_mutex_t   mutex_handle;    /**< RT-Thread mutex handle */
    bool         is_recursive;    /**< True if recursive mutex */
} cy_mutex_t;

/**
 * \brief Thread wrapper structure for RT-Thread
 * Contains thread control block and synchronization primitives
 */
typedef struct
{
    rt_thread_t     thread;             /**< RT-Thread thread handle */
    rt_sem_t        join_sem;           /**< Semaphore for thread join */
    void*           stack;              /**< Pointer to thread stack */
    size_t          stack_size;         /**< Stack size in bytes */
    bool            is_allocated;       /**< True if stack was allocated by us */
    bool            is_valid;           /**< True if thread is valid */
    const char*     name;               /**< Thread name */
} cy_thread_wrapper_t;

/**
 * \brief Timer wrapper structure for RT-Thread
 */
typedef struct
{
    rt_timer_t      timer;              /**< RT-Thread timer handle */
    void            (*callback)(cy_timer_callback_arg_t); /**< User callback function */
    cy_timer_callback_arg_t callback_arg; /**< User callback argument */
    bool            is_periodic;        /**< True if periodic timer */
} cy_timer_wrapper_t;

/**
 * \brief Queue wrapper structure for RT-Thread
 */
typedef struct
{
    rt_mq_t         mq;                 /**< RT-Thread message queue handle */
    size_t          item_size;          /**< Size of each item */
    size_t          max_items;          /**< Maximum number of items */
} cy_queue_wrapper_t;

/**
 * \brief Event wrapper structure for RT-Thread
 */
typedef struct
{
    rt_event_t      event;              /**< RT-Thread event handle */
} cy_event_wrapper_t;

/** Alias for the RTOS specific definition of a thread handle */
typedef cy_thread_wrapper_t* cy_thread_t;

/** Alias for the RTOS specific definition of a semaphore */
typedef rt_sem_t cy_semaphore_t;

/** Alias for the RTOS specific definition of an event */
typedef cy_event_wrapper_t* cy_event_t;

/** Alias for the RTOS specific definition of a message queue */
typedef cy_queue_wrapper_t* cy_queue_t;

/** Alias for the RTOS specific definition of a timer */
typedef cy_timer_wrapper_t* cy_timer_t;

#ifdef __cplusplus
} // extern "C"
#endif
