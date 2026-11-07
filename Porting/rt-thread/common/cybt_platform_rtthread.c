 /*******************************************************************************
* \file cybt_platform_rtthread.c
*
* \brief
* Implementation for BT porting interface on RT-Thread
*
* Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
*                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
* Website: https://www.aerolynq.com - RT-Thread porting
*
* SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
*
* PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
*
* This file is the proprietary work of Aerolynq.  It is NOT released
* under the Apache License, MIT, BSD or any other open-source license.
*
* Use of this file is permitted EXCLUSIVELY in connection with WiFi /
* Bluetooth combo modules manufactured, distributed or otherwise
* authorized by Aerolynq (e.g. CYWL6208 / CYWL6209 / CYWL6302 /
* CYWL6305 / CYWL6612).  Any use, porting, adaptation, redistribution
* or integration of this file with non-Aerolynq WiFi / Bluetooth
* silicon is strictly prohibited and constitutes infringement of
* Aerolynq's copyright and other intellectual-property rights.
*
* See the NOTICE and LICENSE files at the root of this package for the
* complete licensing terms.
*******************************************************************************/

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifdef WHD_USING_BLUETOOTH

#include "wiced_data_types.h"
#include "platform_hal_wrapper.h"
#include "cybt_platform_interface.h"
#include "cybt_platform_task.h"
#include "cybt_platform_trace.h"
#include "cybt_platform_util.h"
#include "cybt_platform_hci.h"
#include "cyabs_rtos.h"

#define DBG_TAG    "cybt.platform"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/******************************************************************************
 *                                Constants
 ******************************************************************************/

/* HCI data trace - enable via menuconfig: WHD_USING_BLUETOOTH -> "Enable HCI data trace"
 * Kconfig bool type defines symbol without value, so we need to handle it properly
 */
#ifdef CYBT_HCI_DATA_TRACE
#undef CYBT_HCI_DATA_TRACE
#define CYBT_HCI_DATA_TRACE     1
#else
#define CYBT_HCI_DATA_TRACE     0
#endif

/* Maximum bytes to print in HCI trace */
#ifndef CYBT_HCI_TRACE_MAX_BYTES
#define CYBT_HCI_TRACE_MAX_BYTES    64
#endif

#define HCI_SEMAPHORE_MAX_COUNT  (1)
#define HCI_SEMAPHORE_INIT_COUNT (0)

/* HCI UART default baud rate */
#ifndef HCI_UART_DEFAULT_BAUDRATE
#define HCI_UART_DEFAULT_BAUDRATE   (115200)
#endif

/******************************************************************************
 *                           Variables Definitions
 ******************************************************************************/

extern hci_uart_cb_t hci_uart_cb;

/* Stack timer for timeout management */
static cy_timer_t stack_timer;
static volatile rt_bool_t platform_sleep_lock = RT_FALSE;

/******************************************************************************
 *                    MCU-Specific Diagnostic Functions
 * 
 * The diagnostic function cybt_platform_hci_uart_diag_dump() is defined
 * as weak in cybt_hci_rx_task.c. To enable MCU-specific diagnostics:
 * 1. Create cybt_platform_diag_<mcu>.c in Porting/platform_mcu/
 * 2. Implement cybt_platform_hci_uart_diag_dump() for your MCU
 * 3. Include the file in your build (the strong definition overrides weak)
 ******************************************************************************/

/* Declaration - defined as weak in btstack cybt_hci_rx_task.c */
extern void cybt_platform_hci_uart_diag_dump(const char *tag);

/******************************************************************************
 *                          HCI Data Trace Functions
 * Enable CYBT_HCI_DATA_TRACE to print full TX/RX HCI packets for debugging.
 ******************************************************************************/
#if CYBT_HCI_DATA_TRACE

/* HCI Packet Types */
#define HCI_PKT_TYPE_CMD    0x01
#define HCI_PKT_TYPE_ACL    0x02
#define HCI_PKT_TYPE_SCO    0x03
#define HCI_PKT_TYPE_EVT    0x04

/* Static state to track current packet type for multi-part reads */
static uint8_t hci_trace_rx_pkt_type = 0;
static uint8_t hci_trace_rx_evt_code = 0;

/**
 * Get packet type name string
 */
static const char* cybt_hci_pkt_type_str(uint8_t pkt_type)
{
    switch (pkt_type)
    {
        case HCI_PKT_TYPE_CMD: return "CMD";
        case HCI_PKT_TYPE_ACL: return "ACL";
        case HCI_PKT_TYPE_SCO: return "SCO";
        case HCI_PKT_TYPE_EVT: return "EVT";
        default: return "???";
    }
}

/**
 * Print HCI TX packet (always starts with packet type byte)
 * @param data  Pointer to data buffer (includes packet type)
 * @param len   Data length
 */
static void cybt_hci_trace_tx(const uint8_t *data, uint32_t len)
{
    static char trace_buf[280];
    uint32_t i;
    uint32_t print_len = (len > CYBT_HCI_TRACE_MAX_BYTES) ? CYBT_HCI_TRACE_MAX_BYTES : len;
    int pos = 0;
    rt_base_t level;
    uint8_t pkt_type = (len > 0) ? data[0] : 0;
    
    /* Build header with packet type */
    pos = rt_snprintf(trace_buf, sizeof(trace_buf), "[HCI TX %s] len=%d:",
                      cybt_hci_pkt_type_str(pkt_type), (int)len);
    
    for (i = 0; i < print_len && pos < (int)(sizeof(trace_buf) - 10); i++)
    {
        pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " %02X", data[i]);
    }
    
    if (len > CYBT_HCI_TRACE_MAX_BYTES)
    {
        pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " ...(+%d)", (int)(len - CYBT_HCI_TRACE_MAX_BYTES));
    }
    
    level = rt_hw_interrupt_disable();
    rt_kprintf("%s\n", trace_buf);
    rt_hw_interrupt_enable(level);
}

/**
 * Print HCI RX packet (may be received in parts: type, header, payload)
 * Combines header+payload into single line where possible
 * @param data  Pointer to data buffer
 * @param len   Data length
 */
static void cybt_hci_trace_rx(const uint8_t *data, uint32_t len)
{
    static char trace_buf[280];
    /* Buffer to accumulate event header+payload for combined print */
    static uint8_t evt_buf[260];
    static uint32_t evt_buf_len = 0;
    static uint32_t evt_expected_len = 0;
    uint32_t i;
    uint32_t print_len;
    int pos = 0;
    rt_base_t level;
    
    /* Skip zero-length reads */
    if (len == 0)
    {
        return;
    }
    
    /* Detect packet type (first byte of new packet) */
    if (len == 1 && (data[0] == HCI_PKT_TYPE_EVT || data[0] == HCI_PKT_TYPE_ACL || data[0] == HCI_PKT_TYPE_SCO))
    {
        /* This is packet type byte, save it but don't print yet */
        hci_trace_rx_pkt_type = data[0];
        hci_trace_rx_evt_code = 0;
        evt_buf_len = 0;
        evt_expected_len = 0;
        return;  /* Wait for header to print combined */
    }
    
    /* For EVT: accumulate header and payload, then print combined */
    if (hci_trace_rx_pkt_type == HCI_PKT_TYPE_EVT)
    {
        /* Accumulate data into buffer */
        if (evt_buf_len + len <= sizeof(evt_buf))
        {
            rt_memcpy(evt_buf + evt_buf_len, data, len);
            evt_buf_len += len;
        }
        
        /* First 2 bytes are event header: event_code + param_len */
        if (evt_buf_len >= 2 && evt_expected_len == 0)
        {
            hci_trace_rx_evt_code = evt_buf[0];
            evt_expected_len = 2 + evt_buf[1];  /* header(2) + param_len */
        }
        
        /* Check if we have complete event packet */
        if (evt_expected_len > 0 && evt_buf_len >= evt_expected_len)
        {
            /* Print complete event packet */
            print_len = (evt_buf_len > CYBT_HCI_TRACE_MAX_BYTES) ? CYBT_HCI_TRACE_MAX_BYTES : evt_buf_len;
            pos = rt_snprintf(trace_buf, sizeof(trace_buf), "[HCI RX EVT 0x%02X] len=%d:",
                              hci_trace_rx_evt_code, (int)evt_buf_len);
            
            for (i = 0; i < print_len && pos < (int)(sizeof(trace_buf) - 10); i++)
            {
                pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " %02X", evt_buf[i]);
            }
            
            if (evt_buf_len > CYBT_HCI_TRACE_MAX_BYTES)
            {
                pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " ...(+%d)", (int)(evt_buf_len - CYBT_HCI_TRACE_MAX_BYTES));
            }
            
            level = rt_hw_interrupt_disable();
            rt_kprintf("%s\n", trace_buf);
            rt_hw_interrupt_enable(level);
            
            /* Reset state */
            hci_trace_rx_pkt_type = 0;
            hci_trace_rx_evt_code = 0;
            evt_buf_len = 0;
            evt_expected_len = 0;
        }
        /* Else: still accumulating, don't print yet */
        return;
    }
    
    /* For ACL/SCO: print directly (they have their own header parsing) */
    print_len = (len > CYBT_HCI_TRACE_MAX_BYTES) ? CYBT_HCI_TRACE_MAX_BYTES : len;
    
    if (hci_trace_rx_pkt_type != 0)
    {
        pos = rt_snprintf(trace_buf, sizeof(trace_buf), "[HCI RX %s] len=%d:",
                          cybt_hci_pkt_type_str(hci_trace_rx_pkt_type), (int)len);
    }
    else
    {
        pos = rt_snprintf(trace_buf, sizeof(trace_buf), "[HCI RX] len=%d:", (int)len);
    }
    
    for (i = 0; i < print_len && pos < (int)(sizeof(trace_buf) - 10); i++)
    {
        pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " %02X", data[i]);
    }
    
    if (len > CYBT_HCI_TRACE_MAX_BYTES)
    {
        pos += rt_snprintf(trace_buf + pos, sizeof(trace_buf) - pos, " ...(+%d)", (int)(len - CYBT_HCI_TRACE_MAX_BYTES));
    }
    
    level = rt_hw_interrupt_disable();
    rt_kprintf("%s\n", trace_buf);
    rt_hw_interrupt_enable(level);
    
    /* Reset packet type after non-EVT packet */
    hci_trace_rx_pkt_type = 0;
}

#define CYBT_HCI_TRACE_TX(data, len)    cybt_hci_trace_tx((const uint8_t*)(data), (len))
#define CYBT_HCI_TRACE_RX(data, len)    cybt_hci_trace_rx((const uint8_t*)(data), (len))

#else /* !CYBT_HCI_DATA_TRACE */

#define CYBT_HCI_TRACE_TX(data, len)    do {} while(0)
#define CYBT_HCI_TRACE_RX(data, len)    do {} while(0)

#endif /* CYBT_HCI_DATA_TRACE */

/******************************************************************************
 *                          Function Declarations
 ******************************************************************************/

static void platform_stack_timer_callback(cy_timer_callback_arg_t arg);

/******************************************************************************
 *                           Function Definitions
 ******************************************************************************/

/**
 * Stack timer callback
 */
static void platform_stack_timer_callback(cy_timer_callback_arg_t arg)
{
    (void)arg;
    
    cybt_platform_sleep_lock();
    cybt_send_msg_to_hci_rx_task(BT_IND_TO_BTS_TIMER, true);
    cybt_platform_sleep_unlock();
}

/**
 * Platform memory allocation
 */
void *cybt_platform_malloc(uint32_t req_size)
{
    return rt_malloc((rt_size_t)req_size);
}

/**
 * Platform memory free
 */
void cybt_platform_free(void *p_mem_block)
{
    if (p_mem_block != RT_NULL)
    {
        rt_free(p_mem_block);
    }
}

/* Static variable to save interrupt level for nested disable/enable */
static volatile rt_base_t s_irq_level = 0;
static volatile rt_uint32_t s_irq_nest = 0;

/**
 * Disable interrupts
 */
void cybt_platform_disable_irq(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_irq_nest == 0)
    {
        s_irq_level = level;
    }
    s_irq_nest++;
}

/**
 * Enable interrupts
 */
void cybt_platform_enable_irq(void)
{
    if (s_irq_nest > 0)
    {
        s_irq_nest--;
        if (s_irq_nest == 0)
        {
            rt_hw_interrupt_enable(s_irq_level);
        }
    }
}

/**
 * Platform initialization
 */
void cybt_platform_init(void)
{
    LOG_I("cybt_platform_init()");
    
    rt_memset(&hci_uart_cb, 0, sizeof(hci_uart_cb_t));
    
    /* Create stack timer using cy_rtos API (same as FreeRTOS) */
    cy_rtos_init_timer(&stack_timer, CY_TIMER_TYPE_ONCE, 
                       platform_stack_timer_callback, (cy_timer_callback_arg_t)NULL);
}

/**
 * Platform de-initialization
 */
void cybt_platform_deinit(void)
{
    LOG_I("cybt_platform_deinit()");
    
    cy_rtos_deinit_timer(&stack_timer);
}

/**
 * Lock sleep mode (prevent system from entering deep sleep)
 * Note: RT-Thread doesn't use tickless idle by default, so this is simplified
 */
void cybt_platform_sleep_lock(void)
{
    cybt_platform_disable_irq();
    
    if (platform_sleep_lock == RT_FALSE)
    {
        platform_hal_syspm_lock_deepsleep_wrapper();
        platform_sleep_lock = RT_TRUE;
    }
    
    cybt_platform_enable_irq();
}

/**
 * Unlock sleep mode
 */
void cybt_platform_sleep_unlock(void)
{
    cybt_platform_disable_irq();
    
    if (platform_sleep_lock == RT_TRUE)
    {
        platform_hal_syspm_unlock_deepsleep_wrapper();
        platform_sleep_lock = RT_FALSE;
    }
    
    cybt_platform_enable_irq();
}

/**
 * Get current tick count in microseconds
 */
uint64_t cybt_platform_get_tick_count_us(void)
{
    static cy_time_t last_time_in_ms = 0;
    static uint64_t abs_time_cnt_in_us_hi = 0;
    cy_time_t cur_time_in_ms;
    uint64_t cur_time_in_ms64 = 0;
    uint64_t cur_time_in_us64 = 0;

    cy_rtos_get_time(&cur_time_in_ms);

    /* Handle time wrap-around */
    if (cur_time_in_ms < last_time_in_ms)
    {
        abs_time_cnt_in_us_hi += 0x100000000ULL;
    }

    last_time_in_ms = cur_time_in_ms;

    cur_time_in_ms64 = cur_time_in_ms + abs_time_cnt_in_us_hi;
    cur_time_in_us64 = (cur_time_in_ms64 * 1000);
    
    return cur_time_in_us64;
}

/**
 * Set next timeout for stack timer
 */
void cybt_platform_set_next_timeout(uint64_t abs_tick_us_to_expire)
{
    uint64_t curr_time_us = cybt_platform_get_tick_count_us();
    uint64_t time_to_expire_in_us;
    
    if (abs_tick_us_to_expire <= curr_time_us)
    {
        /* Already expired */
        cybt_send_msg_to_hci_rx_task(BT_IND_TO_BTS_TIMER, true);
        return;
    }

    time_to_expire_in_us = abs_tick_us_to_expire - curr_time_us;
    
    {
        cy_rslt_t result;
        cy_time_t next_timeout = (cy_time_t)(time_to_expire_in_us / 1000);
        
        if (next_timeout == 0)
            next_timeout = 1;
        
        /* Start timer with new timeout */
        result = cy_rtos_start_timer(&stack_timer, next_timeout);
        if (CY_RSLT_SUCCESS != result)
        {
            LOG_E("timer failed to start %u", next_timeout);
        }
    }
}

/**
 * Weak function for debug UART trace
 */
__attribute__((weak)) cybt_result_t cybt_debug_uart_send_trace(uint16_t length, uint8_t* p_data)
{
    (void)length;
    (void)p_data;
    return CYBT_SUCCESS;
}

/**
 * Platform log print function
 */
void cybt_platform_log_print(const char *fmt_str, ...)
{
    char buffer[CYBT_TRACE_BUFFER_SIZE];
    va_list ap;
    int len;
    rt_tick_t time;
    
    time = rt_tick_get();
    va_start(ap, fmt_str);
    len = rt_vsnprintf(buffer, CYBT_TRACE_BUFFER_SIZE, fmt_str, ap);
    va_end(ap);
    
    rt_kprintf("[%u] %s\r\n", (unsigned int)time, buffer);
    (void)len;
}

/**
 * Assert BT device wake pin (wake up BT chip)
 */
void cybt_platform_assert_bt_wake(void)
{
    bool wake_polarity = platform_hal_gpio_dev_wake_polarity_wrapper();
    
    if (cybt_platform_get_sleep_mode_status() == true)
    {
        platform_hal_gpio_write_wrapper(DEVICE_WAKE_PIN, wake_polarity);
    }
}

/**
 * De-assert BT device wake pin (allow BT chip to sleep)
 */
void cybt_platform_deassert_bt_wake(void)
{
    bool wake_polarity = platform_hal_gpio_dev_wake_polarity_wrapper();
    
    if (cybt_platform_get_sleep_mode_status() == true)
    {
        platform_hal_gpio_write_wrapper(DEVICE_WAKE_PIN, !wake_polarity);
    }
}

/* Note: cy_rtos_delay_milliseconds and cy_rtos_get_time are already 
 * implemented in External/abstraction-rtos/source/COMPONENT_RTTHREAD/cyabs_rtos_rtthread.c */

/******************************************************************************
 *                           HCI Platform Functions
 ******************************************************************************/

/**
 * Open HCI transport (UART)
 * This function initializes all required resources for HCI communication
 */
cybt_result_t cybt_platform_hci_open(void *p_arg)
{
    cybt_result_t result;
    (void)p_arg;

    if (true == hci_uart_cb.inited)
    {
        return CYBT_SUCCESS;
    }

    rt_memset(&hci_uart_cb, 0, sizeof(hci_uart_cb_t));

    /* Initialize semaphores for TX/RX synchronization */
    cy_rtos_init_semaphore(&hci_uart_cb.tx_complete,
                           HCI_SEMAPHORE_MAX_COUNT,
                           HCI_SEMAPHORE_INIT_COUNT);
    cy_rtos_init_semaphore(&hci_uart_cb.rx_complete,
                           HCI_SEMAPHORE_MAX_COUNT,
                           HCI_SEMAPHORE_INIT_COUNT);
    
    /* Initialize mutexes for TX/RX atomic operations */
    cy_rtos_init_mutex(&hci_uart_cb.tx_atomic);
    cy_rtos_init_mutex(&hci_uart_cb.rx_atomic);

    /* Initialize device wake pin if sleep mode is enabled */
    if (CYBT_SLEEP_MODE_ENABLED == platform_hal_get_sleep_mode_enabled_wrapper())
    {
        result = platform_hal_gpio_init_wrapper(DEVICE_WAKE_PIN);
        if (result != CYBT_SUCCESS)
        {
            LOG_E("Failed to init DEVICE_WAKE_PIN: 0x%x", result);
            return result;
        }

        result = platform_hal_gpio_write_wrapper(DEVICE_WAKE_PIN, false);
        if (result != CYBT_SUCCESS)
        {
            LOG_E("Failed to write DEVICE_WAKE_PIN: 0x%x", result);
            return result;
        }
        cy_rtos_delay_milliseconds(100);
    }

    /* Initialize BT power pin */
    result = platform_hal_gpio_init_wrapper(BT_POWER_PIN);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("Failed to init BT_POWER_PIN: 0x%x", result);
        return result;
    }
    LOG_I("BT_POWER_PIN init OK");

    /* Power cycle: First power OFF, then ON to ensure proper reset */
    LOG_I("BT power OFF first (reset)...");
    platform_hal_gpio_write_wrapper(BT_POWER_PIN, false);
    cy_rtos_delay_milliseconds(100);
    
    /* Now power ON */
    result = platform_hal_gpio_write_wrapper(BT_POWER_PIN, true);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("Failed to power on BT: 0x%x", result);
        return result;
    }
    LOG_I("BT power ON");
    
    /* Wait for BT module to stabilize after power on
     * CYW43xxx needs about 50ms after power on before it can respond
     * We use longer delay to be safe
     */
    cy_rtos_delay_milliseconds(500);

    /* Initialize UART */
    result = platform_hal_uart_init_wrapper();
    if (result != CYBT_SUCCESS)
    {
        LOG_E("HCI UART init failed: 0x%x", result);
        return CYBT_ERR_HCI_INIT_FAILED;
    }
    LOG_I("UART init OK");

    /* Set default baud rate */
    result = platform_hal_uart_set_baud_wrapper(HCI_UART_DEFAULT_BAUDRATE);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("HCI set baudrate failed: 0x%x", result);
        return result;
    }
    LOG_I("Baud rate set OK");

    /* Enable hardware flow control */
    result = platform_hal_uart_enable_flow_control_wrapper();
    if (result != CYBT_SUCCESS)
    {
        LOG_E("HCI enable flow control failed: 0x%x", result);
        return result;
    }
    LOG_I("Flow control enabled");

    cybt_platform_hci_uart_diag_dump("open:after_flow_ctrl");

#if CYBT_HCI_DIAG_ENABLED
    /* Diagnostics: sample raw GPIO levels for CTS/RTS */
    {
        uint32_t cts_level = gpio_input_bit_get(CYBT_HCI_CTS_PORT, CYBT_HCI_CTS_PIN);
        uint32_t rts_level = gpio_input_bit_get(CYBT_HCI_RTS_PORT, CYBT_HCI_RTS_PIN);
        LOG_I("HCI DIAG v2: CTS=%d RTS=%d", (int)cts_level, (int)rts_level);
    }
#endif

    /* Register UART callbacks and enable events */
    platform_hal_uart_register_callback_wrapper(true);
    platform_hal_uart_enable_event_wrapper(true);

    LOG_I("hci_open(): Wait CTS low");

    /* Wait for CTS to go low (BT chip ready) with timeout
     * Note: CTS pin is configured as AF mode for UART hardware flow control,
     * so we need to read directly from GPIO input register
     * instead of using rt_pin_read() which may not work in AF mode.
     */
#if CYBT_HCI_DIAG_ENABLED
    {
        int cts_wait_count = 0;
        
        while (gpio_input_bit_get(CYBT_HCI_CTS_PORT, CYBT_HCI_CTS_PIN) == SET)
        {
            cy_rtos_delay_milliseconds(10);
            cts_wait_count++;
            if (cts_wait_count > 300)  /* 3 second timeout */
            {
                LOG_E("hci_open(): CTS timeout!");
                /* Continue anyway - some boards don't have CTS connected */
                break;
            }
        }
        LOG_I("hci_open(): CTS ready (wait=%d)", cts_wait_count);
    }
#else
    /* Without MCU-specific GPIO access, just wait a fixed delay */
    cy_rtos_delay_milliseconds(100);
    LOG_I("hci_open(): CTS wait skipped (no MCU-specific GPIO access)");
#endif

    cybt_platform_hci_uart_diag_dump("open:after_cts_wait");

#if CYBT_HCI_DIAG_ENABLED
    /* Diagnostics: re-sample levels after CTS wait */
    {
        uint32_t cts_level = gpio_input_bit_get(CYBT_HCI_CTS_PORT, CYBT_HCI_CTS_PIN);
        uint32_t rts_level = gpio_input_bit_get(CYBT_HCI_RTS_PORT, CYBT_HCI_RTS_PIN);
        LOG_I("HCI DIAG v2(after wait): CTS=%d RTS=%d", (int)cts_level, (int)rts_level);
    }
#endif

    /* Flush any garbage data in RX buffer before starting HCI communication */
    {
        uint8_t dummy;
        int flushed = 0;
        while (cyhal_uart_readable(&hci_uart_cb.hal_obj) > 0 && flushed < 256)
        {
            cyhal_uart_getc(&hci_uart_cb.hal_obj, &dummy, 0);
            flushed++;
        }
        if (flushed > 0)
        {
            LOG_I("hci_open(): Flushed %d bytes of garbage from RX buffer", flushed);
        }
    }

    /* Additional delay to ensure BT chip is fully ready for HCI commands */
    cy_rtos_delay_milliseconds(200);
    LOG_I("hci_open(): Extra 200ms delay completed");

    hci_uart_cb.inited = true;

    LOG_I("hci_open(): Done");

    return CYBT_SUCCESS;
}

/**
 * Set HCI UART baud rate
 */
cybt_result_t cybt_platform_hci_set_baudrate(uint32_t baudrate)
{
    cybt_result_t result;
    
    if (false == hci_uart_cb.inited)
    {
        LOG_E("set_baudrate(): UART is NOT initialized");
        return CYBT_ERR_HCI_NOT_INITIALIZE;
    }
    
    result = platform_hal_uart_set_baud_wrapper(baudrate);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("HCI set baudrate failed: 0x%x", result);
        return result;
    }

    LOG_D("HCI baudrate set to %u", baudrate);
    return CYBT_SUCCESS;
}

/**
 * Write data to HCI transport
 */
cybt_result_t cybt_platform_hci_write(hci_packet_type_t type,
                                       uint8_t *p_data,
                                       uint32_t length)
{
    cy_rslt_t result;
    cybt_result_t cybt_result = CYBT_SUCCESS;

    (void)type;

    if (false == hci_uart_cb.inited)
    {
        LOG_E("hci_write(): UART is NOT initialized");
        return CYBT_ERR_HCI_NOT_INITIALIZE;
    }

    /* Get TX mutex */
    result = cy_rtos_get_mutex(&hci_uart_cb.tx_atomic, CY_RTOS_NEVER_TIMEOUT);
    if (CY_RSLT_SUCCESS != result)
    {
        LOG_E("hci_write(): Get mutex error (0x%x)", result);
        return CYBT_ERR_HCI_GET_TX_MUTEX_FAILED;
    }

    cybt_platform_sleep_lock();
    cybt_platform_assert_bt_wake();

    /* Write data asynchronously */
    cybt_result = platform_hal_uart_write_async_wrapper((void *)p_data, (size_t)length);
    if (CYBT_SUCCESS == cybt_result)
    {
        /* Wait for TX complete */
        cy_rtos_get_semaphore(&hci_uart_cb.tx_complete, CY_RTOS_NEVER_TIMEOUT, false);
        CYBT_HCI_TRACE_TX(p_data, length);
    }

    cybt_platform_deassert_bt_wake();
    cybt_platform_sleep_unlock();

    cy_rtos_set_mutex(&hci_uart_cb.tx_atomic);

    return cybt_result;
}

/**
 * Read data from HCI transport
 */
cybt_result_t cybt_platform_hci_read(hci_packet_type_t type,
                                      uint8_t *p_data,
                                      uint32_t *p_length,
                                      uint32_t timeout_ms)
{
    uint32_t req_len = *p_length;
    cy_rslt_t result;
    cybt_result_t cybt_result;

    (void)type;  /* Packet type is determined by caller */

    if (false == hci_uart_cb.inited)
    {
        LOG_E("hci_read(): UART is NOT initialized");
        return CYBT_ERR_HCI_NOT_INITIALIZE;
    }

    if (0 == req_len)
    {
        LOG_E("hci_read(): Required length = 0");
        return CYBT_ERR_BADARG;
    }

    /* Get RX mutex with timeout */
    result = cy_rtos_get_mutex(&hci_uart_cb.rx_atomic, timeout_ms);
    if (CY_RSLT_SUCCESS != result)
    {
        LOG_E("hci_read(): Get mutex error (0x%x)", result);
        return CYBT_ERR_HCI_GET_RX_MUTEX_FAILED;
    }

    cybt_platform_sleep_lock();

    if (0 < timeout_ms)
    {
        /* Async read with timeout */
        cybt_result = platform_hal_uart_read_async_wrapper((void *)p_data, (size_t)req_len);
        if (CYBT_SUCCESS == cybt_result)
        {
            result = cy_rtos_get_semaphore(&hci_uart_cb.rx_complete,
                                           timeout_ms,
                                           false);
            if (CY_RSLT_SUCCESS != result)
            {
                LOG_E("hci_read(): failed (0x%x)", result);

                if (CY_RTOS_TIMEOUT == result)
                {
                    cybt_result = CYBT_ERR_TIMEOUT;
                }
                else
                {
                    cybt_result = CYBT_ERR_GENERIC;
                }

                /* Abort the read operation on timeout */
                platform_hal_uart_read_abort_wrapper();
            }
            else
            {
                cybt_result = CYBT_SUCCESS;
                CYBT_HCI_TRACE_RX(p_data, req_len);
            }
        }
        else
        {
            cybt_platform_sleep_unlock();
            cy_rtos_set_mutex(&hci_uart_cb.rx_atomic);
            return cybt_result;
        }
    }
    else
    {
        /* Blocking read (timeout = 0 means use default/blocking) */
        cybt_result = platform_hal_uart_read_wrapper((void *)p_data, (size_t *)p_length);
        if (CYBT_SUCCESS == cybt_result)
        {
            CYBT_HCI_TRACE_RX(p_data, *p_length);
        }
    }

    cybt_platform_sleep_unlock();
    cy_rtos_set_mutex(&hci_uart_cb.rx_atomic);

    return cybt_result;
}

/**
 * Close HCI transport
 */
cybt_result_t cybt_platform_hci_close(void)
{
    cybt_result_t result;

    if (false == hci_uart_cb.inited)
    {
        LOG_E("hci_close(): Not inited");
        return CYBT_ERR_HCI_NOT_INITIALIZE;
    }

    /* Unregister callbacks and disable events */
    platform_hal_uart_register_callback_wrapper(false);
    platform_hal_uart_enable_event_wrapper(false);

    /* Free device wake pin */
    result = platform_hal_gpio_free_wrapper(DEVICE_WAKE_PIN);
    if (result != CYBT_SUCCESS)
    {
        LOG_W("Failed to free DEVICE_WAKE_PIN: 0x%x", result);
    }

    /* Deinitialize mutexes and semaphores */
    cy_rtos_deinit_mutex(&hci_uart_cb.tx_atomic);
    cy_rtos_deinit_mutex(&hci_uart_cb.rx_atomic);
    cy_rtos_deinit_semaphore(&hci_uart_cb.tx_complete);
    cy_rtos_deinit_semaphore(&hci_uart_cb.rx_complete);

    /* Power off BT chip */
    result = platform_hal_gpio_write_wrapper(BT_POWER_PIN, false);
    if (result != CYBT_SUCCESS)
    {
        LOG_W("Failed to power off BT: 0x%x", result);
    }

    result = platform_hal_gpio_free_wrapper(BT_POWER_PIN);
    if (result != CYBT_SUCCESS)
    {
        LOG_W("Failed to free BT_POWER_PIN: 0x%x", result);
    }

    rt_memset(&hci_uart_cb, 0, sizeof(hci_uart_cb_t));

    LOG_D("HCI transport closed");
    return CYBT_SUCCESS;
}

/******************************************************************************
 *                           System HAL Functions
 ******************************************************************************/

/**
 * Enter critical section
 */
uint32_t cyhal_system_critical_section_enter(void)
{
    rt_enter_critical();
    return 0;
}

/**
 * Exit critical section
 */
void cyhal_system_critical_section_exit(uint32_t old_state)
{
    (void)old_state;
    rt_exit_critical();
}

/******************************************************************************
 *                           Power Management Stubs
 ******************************************************************************/

/**
 * Lock deep sleep (prevent system from entering deep sleep)
 */
void cyhal_syspm_lock_deepsleep(void)
{
    /* RT-Thread doesn't have built-in deep sleep management like ModusToolbox
     * Implement if needed for specific power management requirements */
}

/**
 * Unlock deep sleep (allow system to enter deep sleep)
 */
void cyhal_syspm_unlock_deepsleep(void)
{
    /* RT-Thread doesn't have built-in deep sleep management like ModusToolbox
     * Implement if needed for specific power management requirements */
}

#endif /* WHD_USING_BLUETOOTH */
