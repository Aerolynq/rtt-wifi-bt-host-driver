/***********************************************************************************************//**
 * \file cyabs_rtos_rtthread.c
 *
 * \brief
 * Implementation for RT-Thread RTOS abstraction
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

/* RT-Thread 5.2.0+ moved the thread status query into the internal scheduler
 * API (rt_sched_thread_get_stat), which is only declared when the internal
 * source guard is set.  Without this the declaration is invisible to this
 * translation unit and GCC reports an implicit declaration error (Keil only
 * warns).  The same approach is used by the RT-Thread kernel itself, e.g.
 * src/scheduler_up.c and the utest cases.
 * This must be defined before <rtthread.h> pulls in <rtsched.h>. */
#define __RT_IPC_SOURCE__

#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

//#include <cy_utils.h>
#include <cy_result.h>
#include <cyabs_rtos.h>
#include <cyabs_rtos_types.h>

/** Last RTOS error code */
static cy_rtos_error_t last_error = RT_EOK;

/******************************************************
*                 Helper Functions
******************************************************/

/**
 * \brief Thread entry wrapper function
 * This wraps the user's thread entry function to handle thread exit properly
 */
typedef struct
{
    cy_thread_entry_fn_t entry_function;
    cy_thread_arg_t      arg;
    cy_thread_wrapper_t* wrapper;
} thread_entry_data_t;

static void thread_entry_wrapper(void* parameter)
{
    thread_entry_data_t* entry_data = (thread_entry_data_t*)parameter;
    cy_thread_entry_fn_t entry_func = entry_data->entry_function;
    cy_thread_arg_t arg = entry_data->arg;
    cy_thread_wrapper_t* wrapper = entry_data->wrapper;
    
    /* Free entry data as it's no longer needed */
    rt_free(entry_data);
    
    /* Call the actual thread entry function */
    entry_func(arg);
    
    /* Signal that thread has completed */
    if (wrapper->join_sem != RT_NULL)
    {
        rt_sem_release(wrapper->join_sem);
    }

    /* ---- IMPORTANT: Do NOT return from this function. ----
     * If we return, RT-Thread will move the thread into the defunct queue
     * and the idle thread may free the TCB before cy_rtos_thread_terminate()
     * calls rt_thread_delete(), causing an assertion failure:
     *   "rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread"
     *
     * Instead, suspend the current thread so it stays alive (in SUSPEND
     * state) until cy_rtos_thread_terminate() explicitly deletes it.
     */
    rt_thread_suspend(rt_thread_self());
    rt_schedule();
}


//==================================================================================================
// Error Converter
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_last_error
//--------------------------------------------------------------------------------------------------
cy_rtos_error_t cy_rtos_last_error(void)
{
    return last_error;
}


//==================================================================================================
// Threads
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_create
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_create(cy_thread_t* thread, cy_thread_entry_fn_t entry_function,
                                const char* name, void* stack, uint32_t stack_size,
                                cy_thread_priority_t priority, cy_thread_arg_t arg)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (entry_function == NULL) || (stack_size < CY_RTOS_MIN_STACK_SIZE))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else if ((stack != NULL) && (0 != (((rt_ubase_t)stack) & CY_RTOS_ALIGNMENT_MASK)))
    {
        status = CY_RTOS_ALIGNMENT_ERROR;
    }
    else
    {
        /* Allocate thread wrapper structure */
        cy_thread_wrapper_t* wrapper = (cy_thread_wrapper_t*)rt_malloc(sizeof(cy_thread_wrapper_t));
        
        if (wrapper == NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            rt_memset(wrapper, 0, sizeof(cy_thread_wrapper_t));
            
            wrapper->stack_size = stack_size;
            wrapper->name = name;
            
            /* Create join semaphore */
            wrapper->join_sem = rt_sem_create("cy_join", 0, RT_IPC_FLAG_FIFO);
            if (wrapper->join_sem == RT_NULL)
            {
                rt_free(wrapper);
                return CY_RTOS_NO_MEMORY;
            }
            
            /* Allocate entry data structure */
            thread_entry_data_t* entry_data = (thread_entry_data_t*)rt_malloc(sizeof(thread_entry_data_t));
            if (entry_data == NULL)
            {
                rt_sem_delete(wrapper->join_sem);
                rt_free(wrapper);
                return CY_RTOS_NO_MEMORY;
            }
            
            entry_data->entry_function = entry_function;
            entry_data->arg = arg;
            entry_data->wrapper = wrapper;
            
            /* Create the thread */
            if (stack == NULL)
            {
                /* Let RT-Thread allocate the stack */
                wrapper->thread = rt_thread_create(
                    name ? name : "cy_thread",
                    thread_entry_wrapper,
                    entry_data,
                    stack_size,
                    (rt_uint8_t)priority,
                    20  /* time slice */
                );
                wrapper->is_allocated = false;
            }
            else
            {
                /* Use provided stack - need to use static init */
                wrapper->stack = stack;
                wrapper->is_allocated = false;
                
                /* For user-provided stack, we still use dynamic creation
                 * Note: RT-Thread's rt_thread_init requires static thread structure */
                wrapper->thread = rt_thread_create(
                    name ? name : "cy_thread",
                    thread_entry_wrapper,
                    entry_data,
                    stack_size,
                    (rt_uint8_t)priority,
                    20  /* time slice */
                );
            }
            
            if (wrapper->thread == RT_NULL)
            {
                rt_free(entry_data);
                rt_sem_delete(wrapper->join_sem);
                rt_free(wrapper);
                status = CY_RTOS_NO_MEMORY;
            }
            else
            {
                /* Start the thread */
                rt_err_t ret = rt_thread_startup(wrapper->thread);
                if (ret != RT_EOK)
                {
                    last_error = ret;
                    rt_thread_delete(wrapper->thread);
                    rt_free(entry_data);
                    rt_sem_delete(wrapper->join_sem);
                    rt_free(wrapper);
                    status = CY_RTOS_GENERAL_ERROR;
                }
                else
                {
                    wrapper->is_valid = true;
                    *thread = wrapper;
                    status = CY_RSLT_SUCCESS;
                }
            }
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_exit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_exit(void)
{
    /* In RT-Thread, simply returning from the thread function will end the thread.
     * The wrapper function handles signaling the join semaphore. */
    return CY_RSLT_SUCCESS;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_terminate
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_terminate(cy_thread_t* thread)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_thread_wrapper_t* wrapper = *thread;
        
        /* Delete the thread */
        if (wrapper->thread != RT_NULL)
        {
            rt_thread_delete(wrapper->thread);
        }
        
        /* Clean up semaphore */
        if (wrapper->join_sem != RT_NULL)
        {
            rt_sem_delete(wrapper->join_sem);
        }
        
        wrapper->is_valid = false;
        rt_free(wrapper);
        *thread = NULL;
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_is_running
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_is_running(cy_thread_t* thread, bool* running)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL) || (running == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_thread_wrapper_t* wrapper = *thread;
        
        /* Check if thread is the current thread */
        *running = (rt_thread_self() == wrapper->thread);
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_get_state
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_get_state(cy_thread_t* thread, cy_thread_state_t* state)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL) || (state == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_thread_wrapper_t* wrapper = *thread;
        
        if (!wrapper->is_valid || wrapper->thread == RT_NULL)
        {
            *state = CY_THREAD_STATE_TERMINATED;
        }
        else if (rt_thread_self() == wrapper->thread)
        {
            *state = CY_THREAD_STATE_RUNNING;
        }
        else
        {
            /* Get thread state from RT-Thread */
#if (RT_VER_NUM >= 0x50200)
            /* RT-Thread 5.2.0+ uses rt_sched_thread_get_stat() API */
            rt_uint8_t thread_stat = rt_sched_thread_get_stat(wrapper->thread);
#else
            /* RT-Thread 5.1.x and earlier use direct stat field access */
            rt_uint8_t thread_stat = (wrapper->thread->stat & RT_THREAD_STAT_MASK);
#endif
            
            switch (thread_stat)
            {
                case RT_THREAD_INIT:
                    *state = CY_THREAD_STATE_INACTIVE;
                    break;
                case RT_THREAD_READY:
                    *state = CY_THREAD_STATE_READY;
                    break;
                case RT_THREAD_RUNNING:
                    *state = CY_THREAD_STATE_RUNNING;
                    break;
                case RT_THREAD_SUSPEND:
                    *state = CY_THREAD_STATE_BLOCKED;
                    break;
                case RT_THREAD_CLOSE:
                    *state = CY_THREAD_STATE_TERMINATED;
                    break;
                default:
                    *state = CY_THREAD_STATE_UNKNOWN;
                    break;
            }
        }
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_join
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_join(cy_thread_t* thread)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_thread_wrapper_t* wrapper = *thread;
        
        /* Wait for thread to complete */
        rt_err_t ret = rt_sem_take(wrapper->join_sem, RT_WAITING_FOREVER);
        
        if (ret == RT_EOK)
        {
            /* Thread completed, now terminate to clean up */
            status = cy_rtos_thread_terminate(thread);
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_get_handle
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_get_handle(cy_thread_t* thread)
{
    cy_rslt_t status;
    
    if (thread == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        /*
         * Note: This is a workaround for API compatibility.
         * We cast rt_thread_t to cy_thread_t. Functions that use this handle
         * need to be aware that it might not be a valid cy_thread_wrapper_t*.
         */
        *thread = (cy_thread_t)rt_thread_self();
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_wait_notification
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_wait_notification(cy_time_t timeout_ms)
{
    cy_rslt_t status;
    rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
    
    /*
     * Use rt_thread_suspend() with rt_timer for timeout support.
     * When another thread calls rt_thread_resume() on this thread,
     * the thread will wake up.
     */
    rt_thread_t self = rt_thread_self();
    rt_timer_t timeout_timer = RT_NULL;
    
    /* Create a one-shot timer for timeout if needed */
    if (timeout_ms != CY_RTOS_NEVER_TIMEOUT && timeout_ms != 0)
    {
        timeout_timer = rt_timer_create("cy_notif",
                                        (void (*)(void*))rt_thread_resume,
                                        self,
                                        ticks,
                                        RT_TIMER_FLAG_ONE_SHOT);
        if (timeout_timer != RT_NULL)
        {
            rt_timer_start(timeout_timer);
        }
    }
    
    /* Suspend current thread */
    rt_err_t ret = rt_thread_suspend(self);
    
    if (ret != RT_EOK)
    {
        if (timeout_timer != RT_NULL)
        {
            rt_timer_stop(timeout_timer);
            rt_timer_delete(timeout_timer);
        }
        last_error = ret;
        return CY_RTOS_GENERAL_ERROR;
    }
    
    /* Schedule - the thread will be resumed by another thread or timeout timer */
    rt_schedule();
    
    /* Check if we were woken by timeout or notification */
    if (timeout_timer != RT_NULL)
    {
        /* Check if timer is still active (meaning we were woken by notification, not timeout) */
        if ((timeout_timer->parent.flag & RT_TIMER_FLAG_ACTIVATED) != 0)
        {
            /* Timer still running - we were woken by notification */
            rt_timer_stop(timeout_timer);
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            /* Timer expired - timeout occurred */
            status = CY_RTOS_TIMEOUT;
        }
        rt_timer_delete(timeout_timer);
    }
    else
    {
        /* No timeout - must have been notification or infinite wait */
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_set_notification
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_set_notification(cy_thread_t* thread)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        /*
         * The thread pointer could be either:
         * 1. A cy_thread_wrapper_t* from cy_rtos_thread_create
         * 2. A rt_thread_t from cy_rtos_thread_get_handle
         */
        cy_thread_wrapper_t* wrapper = *thread;
        rt_thread_t rt_thread_ptr;
        
        /* Try to determine if this is a wrapper or raw rt_thread_t */
        if (wrapper->is_valid)
        {
            /* It's a wrapper, use the embedded thread */
            rt_thread_ptr = wrapper->thread;
        }
        else
        {
            /* Assume it's a raw rt_thread_t */
            rt_thread_ptr = (rt_thread_t)wrapper;
        }
        
        /* Resume the thread */
        rt_err_t ret = rt_thread_resume(rt_thread_ptr);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_thread_get_name
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_thread_get_name(cy_thread_t* thread, const char** thread_name)
{
    cy_rslt_t status;
    
    if ((thread == NULL) || (*thread == NULL) || (thread_name == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_thread_wrapper_t* wrapper = *thread;
        *thread_name = wrapper->name;
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Scheduler
//==================================================================================================

static volatile uint32_t scheduler_lock_count = 0;

//--------------------------------------------------------------------------------------------------
// cy_rtos_scheduler_suspend
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_scheduler_suspend(void)
{
    rt_enter_critical();
    scheduler_lock_count++;
    return CY_RSLT_SUCCESS;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_scheduler_resume
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_scheduler_resume(void)
{
    cy_rslt_t status;
    
    if (scheduler_lock_count > 0)
    {
        scheduler_lock_count--;
        rt_exit_critical();
        status = CY_RSLT_SUCCESS;
    }
    else
    {
        status = CY_RTOS_BAD_PARAM;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_scheduler_get_state
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_scheduler_get_state(cy_scheduler_state_t* state)
{
    cy_rslt_t status;
    
    if (state == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        if (scheduler_lock_count > 0)
        {
            *state = CY_SCHEDULER_STATE_SUSPENDED;
        }
        else
        {
            /* RT-Thread scheduler is always running after startup */
            *state = CY_SCHEDULER_STATE_RUNNING;
        }
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Mutexes
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_mutex_init
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_mutex_init(cy_mutex_t* mutex, bool recursive)
{
    cy_rslt_t status;
    
    if (mutex == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        mutex->mutex_handle = rt_mutex_create("cy_mutex", RT_IPC_FLAG_PRIO);
        
        if (mutex->mutex_handle == RT_NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            /* RT-Thread mutexes are always recursive */
            mutex->is_recursive = recursive;
            status = CY_RSLT_SUCCESS;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_mutex_get
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_mutex_get(cy_mutex_t* mutex, cy_time_t timeout_ms)
{
    cy_rslt_t status;
    
    if ((mutex == NULL) || (mutex->mutex_handle == RT_NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
        rt_err_t ret = rt_mutex_take(mutex->mutex_handle, ticks);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else if (ret == -RT_ETIMEOUT)
        {
            status = CY_RTOS_TIMEOUT;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_mutex_set
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_mutex_set(cy_mutex_t* mutex)
{
    cy_rslt_t status;
    
    if ((mutex == NULL) || (mutex->mutex_handle == RT_NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_err_t ret = rt_mutex_release(mutex->mutex_handle);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_mutex_deinit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_mutex_deinit(cy_mutex_t* mutex)
{
    cy_rslt_t status;
    
    if (mutex == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        if (mutex->mutex_handle != RT_NULL)
        {
            rt_mutex_delete(mutex->mutex_handle);
            mutex->mutex_handle = RT_NULL;
        }
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Semaphores
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_semaphore_init
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_semaphore_init(cy_semaphore_t* semaphore, uint32_t maxcount, uint32_t initcount)
{
    cy_rslt_t status;
    
    if (semaphore == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        /* Note: RT-Thread semaphores don't have a max count concept in the same way.
         * We create with initial count and let it grow. */
        *semaphore = rt_sem_create("cy_sem", initcount, RT_IPC_FLAG_PRIO);
        
        if (*semaphore == RT_NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            status = CY_RSLT_SUCCESS;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_semaphore_get
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_semaphore_get(cy_semaphore_t* semaphore, cy_time_t timeout_ms)
{
    cy_rslt_t status;
    
    if ((semaphore == NULL) || (*semaphore == RT_NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else if (is_in_isr() && (timeout_ms != 0))
    {
        /* Cannot block in ISR context */
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
        rt_err_t ret = rt_sem_take(*semaphore, ticks);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else if (ret == -RT_ETIMEOUT)
        {
            status = CY_RTOS_TIMEOUT;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_semaphore_set
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_semaphore_set(cy_semaphore_t* semaphore)
{
    cy_rslt_t status;
    
    if ((semaphore == NULL) || (*semaphore == RT_NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_err_t ret = rt_sem_release(*semaphore);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_semaphore_get_count
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_semaphore_get_count(cy_semaphore_t* semaphore, size_t* count)
{
    cy_rslt_t status;
    
    if ((semaphore == NULL) || (*semaphore == RT_NULL) || (count == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        *count = (*semaphore)->value;
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_semaphore_deinit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_semaphore_deinit(cy_semaphore_t* semaphore)
{
    cy_rslt_t status;
    
    if (semaphore == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        if (*semaphore != RT_NULL)
        {
            rt_sem_delete(*semaphore);
            *semaphore = RT_NULL;
        }
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Events
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_event_init
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_init(cy_event_t* event)
{
    cy_rslt_t status;
    
    if (event == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_event_wrapper_t* wrapper = (cy_event_wrapper_t*)rt_malloc(sizeof(cy_event_wrapper_t));
        
        if (wrapper == NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            wrapper->event = rt_event_create("cy_event", RT_IPC_FLAG_PRIO);
            
            if (wrapper->event == RT_NULL)
            {
                rt_free(wrapper);
                status = CY_RTOS_NO_MEMORY;
            }
            else
            {
                *event = wrapper;
                status = CY_RSLT_SUCCESS;
            }
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_event_setbits
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_setbits(cy_event_t* event, uint32_t bits)
{
    cy_rslt_t status;
    
    if ((event == NULL) || (*event == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_event_wrapper_t* wrapper = *event;
        rt_err_t ret = rt_event_send(wrapper->event, bits);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_event_clearbits
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_clearbits(cy_event_t* event, uint32_t bits)
{
    cy_rslt_t status;
    
    if ((event == NULL) || (*event == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_event_wrapper_t* wrapper = *event;
        
        /* RT-Thread doesn't have a direct clear bits function.
         * We need to access the internal set field directly. */
        rt_base_t level = rt_hw_interrupt_disable();
        wrapper->event->set &= ~bits;
        rt_hw_interrupt_enable(level);
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_event_getbits
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_getbits(cy_event_t* event, uint32_t* bits)
{
    cy_rslt_t status;
    
    if ((event == NULL) || (*event == NULL) || (bits == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_event_wrapper_t* wrapper = *event;
        
        /* Read current event bits */
        *bits = wrapper->event->set;
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_event_waitbits
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_waitbits(cy_event_t* event, uint32_t* bits, bool clear, bool all,
                                 cy_time_t timeout_ms)
{
    cy_rslt_t status;
    
    if ((event == NULL) || (*event == NULL) || (bits == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_event_wrapper_t* wrapper = *event;
        rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
        rt_uint32_t wait_bits = *bits;
        rt_uint32_t received = 0;
        rt_uint8_t option = 0;
        
        if (all)
        {
            option |= RT_EVENT_FLAG_AND;
        }
        else
        {
            option |= RT_EVENT_FLAG_OR;
        }
        
        if (clear)
        {
            option |= RT_EVENT_FLAG_CLEAR;
        }
        
        rt_err_t ret = rt_event_recv(wrapper->event, wait_bits, option, ticks, &received);
        
        *bits = received;
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else if (ret == -RT_ETIMEOUT)
        {
            status = CY_RTOS_TIMEOUT;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_event_deinit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_event_deinit(cy_event_t* event)
{
    cy_rslt_t status;
    
    if (event == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        if (*event != NULL)
        {
            cy_event_wrapper_t* wrapper = *event;
            
            if (wrapper->event != RT_NULL)
            {
                rt_event_delete(wrapper->event);
            }
            rt_free(wrapper);
            *event = NULL;
        }
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Queues
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_init
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_init(cy_queue_t* queue, size_t length, size_t itemsize)
{
    cy_rslt_t status;
    
    if (queue == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        /* Allocate queue wrapper */
        cy_queue_wrapper_t* wrapper = (cy_queue_wrapper_t*)rt_malloc(sizeof(cy_queue_wrapper_t));
        
        if (wrapper == NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            wrapper->item_size = itemsize;
            wrapper->max_items = length;
            
            /* Create message queue */
            wrapper->mq = rt_mq_create("cy_mq", itemsize, length, RT_IPC_FLAG_FIFO);
            
            if (wrapper->mq == RT_NULL)
            {
                rt_free(wrapper);
                status = CY_RTOS_NO_MEMORY;
            }
            else
            {
                *queue = wrapper;
                status = CY_RSLT_SUCCESS;
            }
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_put
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_put(cy_queue_t* queue, const void* item_ptr, cy_time_t timeout_ms)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL) || (item_ptr == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        rt_err_t ret;
        
#if defined(RT_USING_MESSAGEQUEUE) && (RTTHREAD_VERSION >= RT_VERSION_CHECK(4, 0, 0))
        /* Use rt_mq_send_wait for RT-Thread 4.0+ which supports timeout */
        rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
        ret = rt_mq_send_wait(wrapper->mq, (void*)item_ptr, wrapper->item_size, ticks);
#else
        /* For older RT-Thread versions, use non-blocking send with retry */
        (void)timeout_ms;  /* Timeout not fully supported in older versions */
        ret = rt_mq_send(wrapper->mq, (void*)item_ptr, wrapper->item_size);
#endif
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else if (ret == -RT_EFULL)
        {
            status = CY_RTOS_QUEUE_FULL;
        }
        else if (ret == -RT_ETIMEOUT)
        {
            status = CY_RTOS_QUEUE_FULL;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_get
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_get(cy_queue_t* queue, void* item_ptr, cy_time_t timeout_ms)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL) || (item_ptr == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        rt_int32_t ticks = convert_ms_to_ticks(timeout_ms);
        
        /* RT-Thread 5.0: rt_mq_recv returns rt_ssize_t
         * - Success: returns message length (positive value)
         * - Failure: returns negative error code (-RT_ETIMEOUT, -RT_ERROR, etc.)
         */
        rt_ssize_t ret = rt_mq_recv(wrapper->mq, item_ptr, wrapper->item_size, ticks);
        
        if (ret > 0)
        {
            /* Success: received message with length = ret */
            status = CY_RSLT_SUCCESS;
        }
        else if (ret == -RT_ETIMEOUT)
        {
            status = CY_RTOS_QUEUE_EMPTY;
        }
        else
        {
            /* ret <= 0 and not timeout is an error */
            last_error = (cy_rtos_error_t)ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_count
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_count(cy_queue_t* queue, size_t* num_waiting)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL) || (num_waiting == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        *num_waiting = wrapper->mq->entry;
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_space
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_space(cy_queue_t* queue, size_t* num_spaces)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL) || (num_spaces == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        *num_spaces = wrapper->max_items - wrapper->mq->entry;
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_reset
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_reset(cy_queue_t* queue)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        
        /* Reset by draining all messages */
        rt_uint8_t* temp = (rt_uint8_t*)rt_malloc(wrapper->item_size);
        if (temp != NULL)
        {
            /* RT-Thread 5.0: rt_mq_recv returns message length (>0) on success */
            while (rt_mq_recv(wrapper->mq, temp, wrapper->item_size, 0) > 0)
            {
                /* Keep draining */
            }
            rt_free(temp);
        }
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_queue_deinit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_queue_deinit(cy_queue_t* queue)
{
    cy_rslt_t status;
    
    if ((queue == NULL) || (*queue == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_queue_wrapper_t* wrapper = *queue;
        
        if (wrapper->mq != RT_NULL)
        {
            rt_mq_delete(wrapper->mq);
        }
        rt_free(wrapper);
        *queue = NULL;
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Timers
//==================================================================================================

/**
 * \brief Timer expiry callback wrapper
 */
static void timer_expiry_callback(void* parameter)
{
    cy_timer_wrapper_t* wrapper = (cy_timer_wrapper_t*)parameter;
    
    if ((wrapper != NULL) && (wrapper->callback != NULL))
    {
        wrapper->callback(wrapper->callback_arg);
    }
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_timer_init
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_timer_init(cy_timer_t* timer, cy_timer_trigger_type_t type,
                             cy_timer_callback_t fun, cy_timer_callback_arg_t arg)
{
    cy_rslt_t status;
    
    if ((timer == NULL) || (fun == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_timer_wrapper_t* wrapper = (cy_timer_wrapper_t*)rt_malloc(sizeof(cy_timer_wrapper_t));
        
        if (wrapper == NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
        else
        {
            wrapper->callback = fun;
            wrapper->callback_arg = arg;
            wrapper->is_periodic = (type == CY_TIMER_TYPE_PERIODIC);
            
            rt_uint8_t flag = (type == CY_TIMER_TYPE_PERIODIC) ? 
                              RT_TIMER_FLAG_PERIODIC : RT_TIMER_FLAG_ONE_SHOT;
            flag |= RT_TIMER_FLAG_SOFT_TIMER;
            
            wrapper->timer = rt_timer_create("cy_timer",
                                             timer_expiry_callback,
                                             wrapper,
                                             1,  /* Initial tick, will be set on start */
                                             flag);
            
            if (wrapper->timer == RT_NULL)
            {
                rt_free(wrapper);
                status = CY_RTOS_NO_MEMORY;
            }
            else
            {
                *timer = wrapper;
                status = CY_RSLT_SUCCESS;
            }
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_timer_start
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_timer_start(cy_timer_t* timer, cy_time_t num_ms)
{
    cy_rslt_t status;
    
    if ((timer == NULL) || (*timer == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_timer_wrapper_t* wrapper = *timer;
        rt_tick_t ticks = rt_tick_from_millisecond(num_ms);
        
        /* Stop timer if running */
        rt_timer_stop(wrapper->timer);
        
        /* Set new timeout value */
        rt_timer_control(wrapper->timer, RT_TIMER_CTRL_SET_TIME, &ticks);
        
        /* Start the timer */
        rt_err_t ret = rt_timer_start(wrapper->timer);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_timer_stop
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_timer_stop(cy_timer_t* timer)
{
    cy_rslt_t status;
    
    if ((timer == NULL) || (*timer == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_timer_wrapper_t* wrapper = *timer;
        rt_err_t ret = rt_timer_stop(wrapper->timer);
        
        if (ret == RT_EOK)
        {
            status = CY_RSLT_SUCCESS;
        }
        else
        {
            last_error = ret;
            status = CY_RTOS_GENERAL_ERROR;
        }
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_timer_is_running
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_timer_is_running(cy_timer_t* timer, bool* state)
{
    cy_rslt_t status;
    
    if ((timer == NULL) || (*timer == NULL) || (state == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_timer_wrapper_t* wrapper = *timer;
        
        /* Check timer state */
        *state = ((wrapper->timer->parent.flag & RT_TIMER_FLAG_ACTIVATED) != 0);
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_timer_deinit
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_timer_deinit(cy_timer_t* timer)
{
    cy_rslt_t status;
    
    if ((timer == NULL) || (*timer == NULL))
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        cy_timer_wrapper_t* wrapper = *timer;
        
        rt_timer_stop(wrapper->timer);
        rt_timer_delete(wrapper->timer);
        rt_free(wrapper);
        *timer = NULL;
        
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//==================================================================================================
// Time
//==================================================================================================

//--------------------------------------------------------------------------------------------------
// cy_rtos_time_get
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_time_get(cy_time_t* tval)
{
    cy_rslt_t status;
    
    if (tval == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
#if (RTTHREAD_VERSION >= RT_VERSION_CHECK(4, 1, 0))
        *tval = (cy_time_t)rt_tick_get_millisecond();
#else
        /* For older RT-Thread versions, calculate manually */
        *tval = (cy_time_t)(rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
#endif
        status = CY_RSLT_SUCCESS;
    }
    
    return status;
}


//--------------------------------------------------------------------------------------------------
// cy_rtos_delay_milliseconds
//--------------------------------------------------------------------------------------------------
cy_rslt_t cy_rtos_delay_milliseconds(cy_time_t num_ms)
{
    rt_thread_mdelay(num_ms);
    return CY_RSLT_SUCCESS;
}

//#ifdef WHD_USE_CUSTOM_MALLOC_IMPL
/* Use the system's built-in memory allocation */
/*
void *whd_mem_malloc (size_t size)
{
    return rt_malloc(size);
}

void *whd_mem_calloc(size_t nitems, size_t size)
{
    return rt_calloc(nitems, size);
}

void whd_mem_free(void *ptr)
{
    rt_free(ptr);
}

void whd_mem_memcpy(void *dest, const void *src, size_t len)
{
    rt_memcpy(dest, src, len);
}

void whd_mem_memset(void *buf, int val, size_t len)
{
    rt_memset(buf, val, len);
}
*/
//#endif /* ifndef WHD_USE_CUSTOM_MALLOC_IMPL */
