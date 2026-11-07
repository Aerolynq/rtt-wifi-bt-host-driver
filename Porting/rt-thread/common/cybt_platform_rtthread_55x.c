/*******************************************************************************
* \file cybt_platform_rtthread_55x.c
*
* \brief
* Implementation for BT porting interface on RT-Thread for 55x series chips
* (CYW55512/CYW55513/CYW55572/CYWL6612)
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

/* Enable 55x (WiFi6) specific features:
 * - Enter Download Mode command instead of autobaud
 * - Uses firmware download baud rate instead of default HCI baud rate
 */
#define COMPONENT_55X

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

#define DBG_TAG    "cybt.55x"
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

/* Enter Download Mode response length */
#define ENTER_DOWNLOAD_MODE_RES_LEN     29

/* HCI Reset command (used to sync the controller before changing baud rate) */
#define HCI_RESET_CMD_LEN               4

/* Vendor specific UPDATE_BAUDRATE command (0xFC18) */
#define HCI_VSC_UPDATE_BAUDRATE_CMD_LEN     10
#define HCI_VSC_UPDATE_BAUDRATE_RES_LEN     7

/* HCI UART default baud rate - Note: 55x uses firmware download baud instead */
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

/* Baud rate the BT controller (chip) is currently running at.
 *
 * The controller only changes its UART baud rate when it is sent an
 * UPDATE_BAUDRATE (0xFC18) vendor specific command, and it does so after
 * the command complete response has been sent at the old rate. The host
 * therefore has to mirror every one of those changes, and must never change
 * its own rate on its own - that would desynchronise host and controller.
 *
 * The controller powers up at HCI_UART_DEFAULT_BAUDRATE.
 */
static uint32_t  s_controller_baud = 0;

/* Set once the firmware has been launched (LAUNCH_RAM, opcode 0xFC4E).
 *
 * After LAUNCH_RAM the controller runs the downloaded application and keeps
 * the baud rate that was used for the download. Infineon's reference tool
 * (mbt.c, execute_download_threeM()) states for H1/55x:
 *   "After download success, the controller baudrate will keep in 3M"
 */
static rt_bool_t s_fw_launched = RT_FALSE;

/******************************************************************************
 *                    MCU-Specific Diagnostic Functions
 * 
 * The diagnostic function is defined as weak in cybt_hci_rx_task.c,
 * allowing MCU-specific implementations to override it.
 * See platform_mcu/cybt_platform_diag_xxx.c for example implementations.
 ******************************************************************************/

/* Declaration - defined as weak in btstack cybt_hci_rx_task.c */
extern void cybt_platform_hci_uart_diag_dump(const char *tag);

/******************************************************************************
 *                          HCI Data Trace Helper
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
    LOG_I("cybt_platform_init() [55x series]");
    
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
    LOG_I("cybt_platform_deinit() [55x series]");
    
    cy_rtos_deinit_timer(&stack_timer);
}

/**
 * Lock sleep mode (prevent system from entering deep sleep)
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

/******************************************************************************
 *                    55x-Specific Functions (Enter Download Mode)
 ******************************************************************************/

#ifdef COMPONENT_55X
/**
   * Send an HCI command on the UART and wait for the Command Complete event
   *
   * The command is written directly on the UART (bypassing the BT stack
   * command path) because this is used during hci_open(), before the stack
   * is able to process HCI commands.
   *
   * The response is a HCI Command Complete event:
   *
   *   04 0E 04 01 <opcode_lo> <opcode_hi> <status>
   *   |  |  |  |  |
   *   |  |  |  |  +-- opcode little endian (e.g. 18 FC -> 0xFC18)
   *   |  |  |  +----- number of HCI command packets
   *   |  |  +-------- parameter length
   *   |  +----------- event code (0x0E = Command Complete)
   *   +-------------- HCI packet type (0x04 = event)
   *
   * Unsolicited events may be received in between, so the buffer is scanned
   * for a matching Command Complete event.
   *
   * @param[in]  p_cmd       : command buffer to send
   * @param[in]  cmd_len     : length of the command
   * @param[out] p_status    : status byte of the Command Complete event
   * @param[in]  timeout_ms  : timeout waiting for the response
   *
   * @return CYBT_SUCCESS if the Command Complete event was received
   */
static cybt_result_t cybt_platform_send_hci_cmd_raw(uint8_t *p_cmd,
                                                    size_t cmd_len,
                                                    uint8_t *p_status,
                                                    uint32_t timeout_ms)
{
    uint8_t opcode_lo = p_cmd[1];
    uint8_t opcode_hi = p_cmd[2];
    size_t tx_size = cmd_len;
    size_t read_len;
    size_t used = 0;
    rt_tick_t start_tick = rt_tick_get();
    uint32_t timeout_ticks = rt_tick_from_millisecond(timeout_ms);
    uint8_t buf[32];
    cy_rslt_t res;

    res = cyhal_uart_write(&hci_uart_cb.hal_obj, (void *)p_cmd, &tx_size);
    if (res != CY_RSLT_SUCCESS)
    {
        LOG_E("55x: Failed to send HCI command: 0x%x", res);
        return (cybt_result_t)res;
    }

    if (tx_size != cmd_len)
    {
        LOG_E("55x: HCI command short write: %d/%d", (int)tx_size, (int)cmd_len);
        return CYBT_ERR_GENERIC;
    }

    /* Wait for the Command Complete event matching the command opcode */
    while ((rt_tick_get() - start_tick) < timeout_ticks)
    {
        read_len = sizeof(buf) - used;

        res = cyhal_uart_read(&hci_uart_cb.hal_obj, (void *)&buf[used], &read_len);
        if (res != CY_RSLT_SUCCESS)
        {
            LOG_E("55x: HCI command read error: 0x%x", res);
            return (cybt_result_t)res;
        }

        if (read_len > 0)
        {
            used += read_len;

            /* Scan the accumulated data for the Command Complete event */
            if (used >= HCI_VSC_UPDATE_BAUDRATE_RES_LEN)
            {
                size_t i;
                bool matched = false;

                for (i = 0; i <= (used - HCI_VSC_UPDATE_BAUDRATE_RES_LEN); i++)
                {
                    if ((buf[i] == 0x04) && (buf[i + 1] == 0x0E) &&
                        (buf[i + 4] == opcode_lo) && (buf[i + 5] == opcode_hi))
                    {
                        *p_status = buf[i + 6];
                        matched = true;
                        break;
                    }
                }

                if (matched)
                {
                    return CYBT_SUCCESS;
                }

                /* No match yet. Discard the data but keep the last bytes,
                 * they may be the beginning of the expected event. */
                {
                    const size_t keep = HCI_VSC_UPDATE_BAUDRATE_RES_LEN - 1;

                    if (used > keep)
                    {
                        rt_memmove(&buf[0], &buf[used - keep], keep);
                        used = keep;
                    }
                }
            }
        }
        else
        {
            cy_rtos_delay_milliseconds(10);
        }
    }

    LOG_E("55x: No Command Complete event for opcode 0x%02x%02x",
          opcode_hi, opcode_lo);

    return CYBT_ERR_TIMEOUT;
}

/**
   * Negotiate the controller baud rate with the BT controller
   *
   * The controller always powers up at 115200 baud. To download the firmware
   * at a higher baud rate (e.g. 3 Mbps), the controller must first be told to
   * switch its UART baud rate using the vendor specific UPDATE_BAUDRATE
   * command (HCI VSC 0xFC18). Only after the controller acknowledged the
   * command may the host side switch its own UART baud rate.
   *
   * This is the exact sequence used by the Infineon reference tool (mbt.c,
   * execute_download_threeM()):
   *
   *   1. HCI Reset (01 03 0C 00)              -> 115200 baud
   *   2. UPDATE_BAUDRATE (01 18 FC 06 ...)    -> 115200 baud
   *   3. Host switches to the new baud rate
   *   4. Enter Download Mode                   -> new baud rate
   *
   * @param[in] baudrate : target baud rate (must differ from 115200)
   *
   * @return CYBT_SUCCESS if the controller accepted the new baud rate
   */
static cybt_result_t cybt_platform_negotiate_baudrate(uint32_t baudrate)
{
    static uint8_t hci_reset[HCI_RESET_CMD_LEN] = { 0x01, 0x03, 0x0C, 0x00 };
    uint8_t update_baudrate[HCI_VSC_UPDATE_BAUDRATE_CMD_LEN] =
    {
        0x01, 0x18, 0xFC, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t status = 0;
    cybt_result_t result;

    /* Step 1: HCI Reset - the controller must be responsive before the VSC */
    LOG_I("55x: Baud negotiation: sending HCI Reset at %u", HCI_UART_DEFAULT_BAUDRATE);

    result = cybt_platform_send_hci_cmd_raw(hci_reset,
                                            sizeof(hci_reset),
                                            &status,
                                            500);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("55x: Baud negotiation: HCI Reset failed (0x%x)", result);
        return result;
    }
    LOG_I("55x: Baud negotiation: HCI Reset acknowledged");

    /* Step 2: UPDATE_BAUDRATE - baud rate is transferred little endian */
    update_baudrate[6] = baudrate & 0xFF;
    update_baudrate[7] = (baudrate >> 8) & 0xFF;
    update_baudrate[8] = (baudrate >> 16) & 0xFF;
    update_baudrate[9] = (baudrate >> 24) & 0xFF;

    LOG_I("55x: Baud negotiation: sending UPDATE_BAUDRATE (%u) at %u",
          baudrate, HCI_UART_DEFAULT_BAUDRATE);

    result = cybt_platform_send_hci_cmd_raw(update_baudrate,
                                            sizeof(update_baudrate),
                                            &status,
                                            500);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("55x: Baud negotiation: UPDATE_BAUDRATE failed (0x%x)", result);
        return result;
    }

    /* The status byte of the Command Complete event (0x00 = success) */
    if (status != 0x00)
    {
        LOG_E("55x: Baud negotiation: UPDATE_BAUDRATE rejected (status 0x%x)", status);
        return CYBT_ERR_GENERIC;
    }

    /* The controller acknowledged at the old rate and has now switched to the
     * new one. The host must follow - see cybt_platform_hci_set_baudrate(). */
    s_controller_baud = baudrate;

    LOG_I("55x: Baud negotiation: controller switched to %u", baudrate);

    return CYBT_SUCCESS;
}

/**
   * Switch the controller to a new baud rate and follow with the host.
   *
   * The controller only changes its UART rate when it receives an
   * UPDATE_BAUDRATE (0xFC18) command, and it answers that command at its old
   * rate before switching. The host may therefore only change its own rate
   * after the controller has been switched.
   *
   * @param[in] baudrate : new baud rate for both controller and host
   *
   * @return CYBT_SUCCESS if both sides run at the new rate afterwards
   */
static cybt_result_t cybt_platform_switch_baudrate(uint32_t baudrate)
{
    uint8_t update_baudrate[HCI_VSC_UPDATE_BAUDRATE_CMD_LEN] =
    {
        0x01, 0x18, 0xFC, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t status = 0;
    cybt_result_t result;

    if (baudrate == s_controller_baud)
    {
        /* Already there - only the host may have to follow. */
        return platform_hal_uart_set_baud_wrapper(baudrate);
    }

    update_baudrate[6] = baudrate & 0xFF;
    update_baudrate[7] = (baudrate >> 8) & 0xFF;
    update_baudrate[8] = (baudrate >> 16) & 0xFF;
    update_baudrate[9] = (baudrate >> 24) & 0xFF;

    LOG_I("55x: Switching controller baud rate %u -> %u",
          s_controller_baud, baudrate);

    result = cybt_platform_send_hci_cmd_raw(update_baudrate,
                                            sizeof(update_baudrate),
                                            &status,
                                            500);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("55x: UPDATE_BAUDRATE to %u failed (0x%x)", baudrate, result);
        return result;
    }

    if (status != 0x00)
    {
        LOG_E("55x: UPDATE_BAUDRATE to %u rejected (status 0x%x)",
              baudrate, status);
        return CYBT_ERR_GENERIC;
    }

    /* The controller answered at the old rate and switched afterwards. */
    s_controller_baud = baudrate;

    /* Now the host may follow. */
    return platform_hal_uart_set_baud_wrapper(baudrate);
}

/**
   * Enter Download Mode for 55x series chips
   * This is required for CYW55512/CYW55513/CYW55572 (WiFi 6) chips
   * instead of using autobaud mechanism.
   * 
   * The Enter Download Mode command puts the chip into firmware download mode.
   */
cybt_result_t cybt_platform_enter_download_mode(void)
{
    static uint8_t enter_download_mode[] = {
        0x01, 0xED, 0xFF, 0x14, 
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 
        0xED, 0xFF, 0x01, 0x00
    };
    size_t tx_size = sizeof(enter_download_mode);
    uint8_t rx_data[ENTER_DOWNLOAD_MODE_RES_LEN];
    uint8_t total_read_bytes = 0;
    size_t required_len = ENTER_DOWNLOAD_MODE_RES_LEN;
    cy_rslt_t res;
    int retry_count = 0;
    const int max_retries = 50;  /* 500ms timeout (50 * 10ms) */
    
    LOG_I("55x: Sending Enter Download Mode command...");
    
    /* Send Enter Download Mode command */
    res = cyhal_uart_write(&hci_uart_cb.hal_obj, (void *)enter_download_mode, &tx_size);
    if (res != CY_RSLT_SUCCESS)
    {
        LOG_E("55x: Failed to send Enter Download Mode command: 0x%x", res);
        return (cybt_result_t)res;
    }
    
    LOG_D("55x: Sent %d bytes, waiting for response...", (int)tx_size);
    
    /* Wait for response */
    while (total_read_bytes < ENTER_DOWNLOAD_MODE_RES_LEN && retry_count < max_retries)
    {
        cy_rtos_delay_milliseconds(10);
        
        required_len = ENTER_DOWNLOAD_MODE_RES_LEN - total_read_bytes;
        res = cyhal_uart_read(&hci_uart_cb.hal_obj, 
                              (uint8_t *)(rx_data + total_read_bytes), 
                              &required_len);
        if (res != CY_RSLT_SUCCESS)
        {
            LOG_E("55x: Read error: 0x%x", res);
            return (cybt_result_t)res;
        }
        
        if (required_len > 0)
        {
            total_read_bytes += required_len;
            LOG_D("55x: Received %d bytes, total %d/%d", 
                  (int)required_len, (int)total_read_bytes, ENTER_DOWNLOAD_MODE_RES_LEN);
        }
        
        retry_count++;
    }
    
    if (total_read_bytes < ENTER_DOWNLOAD_MODE_RES_LEN)
    {
        LOG_W("55x: Enter Download Mode response incomplete: %d/%d bytes", 
              (int)total_read_bytes, ENTER_DOWNLOAD_MODE_RES_LEN);
        /* Continue anyway - some chips may respond with fewer bytes */
    }
    else
    {
        LOG_I("55x: Enter Download Mode completed successfully");
    }
    
    return CYBT_SUCCESS;
}
#endif /* COMPONENT_55X */

/******************************************************************************
 *                           HCI Platform Functions
 ******************************************************************************/

/**
 * Open HCI transport (UART) for 55x series
 * This function initializes all required resources for HCI communication.
 * For 55x chips, it uses Enter Download Mode instead of autobaud.
 */
cybt_result_t cybt_platform_hci_open(void *p_arg)
{
    cybt_result_t result;
    uint32_t platform_baud_download;
    (void)p_arg;

    if (true == hci_uart_cb.inited)
    {
        return CYBT_SUCCESS;
    }

    /* Get firmware download baud rate for 55x chips */
    platform_baud_download = platform_hal_get_fw_download_baud_wrapper();
    LOG_I("55x: FW download baud rate: %u", platform_baud_download);

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
    LOG_I("55x: BT power OFF first (reset)...");
    platform_hal_gpio_write_wrapper(BT_POWER_PIN, false);
    cy_rtos_delay_milliseconds(100);
    
    /* Now power ON */
    result = platform_hal_gpio_write_wrapper(BT_POWER_PIN, true);
    if (result != CYBT_SUCCESS)
    {
        LOG_E("Failed to power on BT: 0x%x", result);
        return result;
    }
    LOG_I("55x: BT power ON");
    
    /* Wait for BT module to stabilize after power on
     * 55x chips need more time to stabilize
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

    /* Enable hardware flow control */
    result = platform_hal_uart_enable_flow_control_wrapper();
    if (result != CYBT_SUCCESS)
    {
        LOG_E("HCI enable flow control failed: 0x%x", result);
        return result;
    }
    LOG_I("Flow control enabled");

    cybt_platform_hci_uart_diag_dump("55x:open:after_flow_ctrl");

    /* Register UART callbacks and enable events */
    platform_hal_uart_register_callback_wrapper(true);
    platform_hal_uart_enable_event_wrapper(true);

    LOG_I("55x: hci_open(): Wait CTS low");

    /* Wait for CTS to go low (BT chip ready) with timeout */
#if CYBT_HCI_DIAG_ENABLED
    {
        int cts_wait_count = 0;
        
        while (gpio_input_bit_get(CYBT_HCI_CTS_PORT, CYBT_HCI_CTS_PIN) == SET)
        {
            cy_rtos_delay_milliseconds(10);
            cts_wait_count++;
            if (cts_wait_count > 300)  /* 3 second timeout */
            {
                LOG_E("55x: hci_open(): CTS timeout!");
                break;
            }
        }
        LOG_I("55x: hci_open(): CTS ready (wait=%d)", cts_wait_count);
    }
#else
    /* Without MCU-specific GPIO access, wait fixed delay */
    cy_rtos_delay_milliseconds(200);
    LOG_I("55x: hci_open(): CTS wait delay completed");
#endif

    cybt_platform_hci_uart_diag_dump("55x:open:after_cts_wait");

#ifdef COMPONENT_55X
    /* The controller powers up at the default baud rate and stays there until
     * it is told otherwise. Track that, so the host never changes its own
     * rate without the controller having been switched first. */
    s_controller_baud = HCI_UART_DEFAULT_BAUDRATE;

    /* Switch the controller AND the host to the firmware download baud rate.
     *
     * The controller always powers up at 115200 baud. It must first be told
     * to switch baud rate (HCI VSC 0xFC18 UPDATE_BAUDRATE) while it is still
     * running at 115200, and only afterwards may the host UART switch to the
     * new baud rate.
     *
     * Switching the host UART first (as was done previously) makes the host
     * send the Enter Download Mode command at a baud rate the controller does
     * not understand, so the controller never enters download mode.
     */
    if (platform_baud_download != HCI_UART_DEFAULT_BAUDRATE)
    {
        result = cybt_platform_negotiate_baudrate(platform_baud_download);
        if (result != CYBT_SUCCESS)
        {
            /* Fall back to the default baud rate. The controller stays at
             * 115200 as well, so the download still works, only slower. */
            LOG_W("55x: Baud negotiation to %u failed (0x%x), falling back to %u",
                  platform_baud_download, result, HCI_UART_DEFAULT_BAUDRATE);

            result = platform_hal_uart_set_baud_wrapper(HCI_UART_DEFAULT_BAUDRATE);
            if (result != CYBT_SUCCESS)
            {
                LOG_E("HCI set baudrate failed: 0x%x", result);
                return result;
            }
        }
        else
        {
            /* The controller is now at the download baud rate, switch the host */
            result = platform_hal_uart_set_baud_wrapper(platform_baud_download);
            if (result != CYBT_SUCCESS)
            {
                LOG_E("HCI set baudrate failed: 0x%x", result);
                return result;
            }
            LOG_I("55x: Host baud rate set to %u for FW download", platform_baud_download);
        }
    }
    else
    {
        LOG_I("55x: FW download baud rate is %u, no negotiation needed",
              platform_baud_download);
    }

    /* Send Enter Download Mode command for 55x chips.
     * Must be issued after the CTS wait completes; the chip is not yet
     * ready to accept this command while CTS is still high.
     */
    result = cybt_platform_enter_download_mode();
    if (result != CYBT_SUCCESS)
    {
        LOG_W("55x: Enter Download Mode returned 0x%x (continuing anyway)", result);
        /* Don't fail - some chips may still work */
    }
#endif /* COMPONENT_55X */

    /* Flush any garbage data in RX buffer */
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
            LOG_I("55x: hci_open(): Flushed %d bytes of garbage from RX buffer", flushed);
        }
    }

    /* Additional delay to ensure BT chip is fully ready for HCI commands */
    cy_rtos_delay_milliseconds(200);
    LOG_I("55x: hci_open(): Extra 200ms delay completed");

    hci_uart_cb.inited = true;

    LOG_I("55x: hci_open(): Done");

    return CYBT_SUCCESS;
}

/**
 * Set HCI UART baud rate
 *
 * The host UART baud rate must always match the baud rate the BT controller
 * is running at. The only entity allowed to change the controller rate is
 * the UPDATE_BAUDRATE (0xFC18) command, so the host may only follow such a
 * change - never initiate one on its own.
 *
 * Infineon's cybt_patchram_download_55x.c calls this with
 * HCI_UART_DEFAULT_BAUDRATE after the firmware has been launched, assuming
 * the controller had reset back to 115200 by then. That assumption does not
 * hold for 55x: the controller keeps the download baud rate after LAUNCH_RAM
 * (Infineon reference tool mbt.c: "After download success, the controller
 * baudrate will keep in 3M"). Changing only the host would leave it at
 * 115200 while the controller is still at the download rate, and the
 * following UPDATE_BAUDRATE command would be decoded as garbage - the
 * controller would never answer it and the BT stack would hang.
 *
 * Therefore a request for a rate the controller does not run at is served
 * by switching the controller first (0xFC18) and letting the host follow
 * right after. This keeps both sides synchronised for any combination of
 * download and feature baud rate.
 */
cybt_result_t cybt_platform_hci_set_baudrate(uint32_t baudrate)
{
    cybt_result_t result;

    if (false == hci_uart_cb.inited)
    {
        LOG_E("set_baudrate(): UART is NOT initialized");
        return CYBT_ERR_HCI_NOT_INITIALIZE;
    }

    /* The controller must be told to switch rate first (0xFC18). Until that
     * happened the controller is still running at s_controller_baud, so the
     * host must not leave that rate on its own.
     *
     * Infineon's stack calls this with HCI_UART_DEFAULT_BAUDRATE after the
     * firmware was launched, assuming the controller had reset back to 115200
     * by then. That assumption does not hold for 55x - the controller keeps
     * the download rate (Infineon reference tool mbt.c: "After download
     * success, the controller baudrate will keep in 3M"). Switching only the
     * host would desynchronise both sides: the following UPDATE_BAUDRATE
     * command would be decoded as garbage, the controller would never answer
     * it and the BT stack would hang.
     *
     * Instead the controller is switched first and the host follows right
     * after, which keeps both sides synchronised for any combination of
     * download and feature baud rate. */
    if (baudrate != s_controller_baud)
    {
        cybt_result_t sync_result;

        if (s_fw_launched)
        {
            /* After LAUNCH_RAM the controller runs the downloaded application
             * and keeps the download rate, so the switch has to be done
             * explicitly on both sides. */
            LOG_I("55x: Switching controller and host to %u after FW launch",
                  baudrate);
        }

        sync_result = cybt_platform_switch_baudrate(baudrate);
        if (sync_result != CYBT_SUCCESS)
        {
            LOG_E("55x: set_baudrate(%u) failed (0x%x)", baudrate, sync_result);
            return sync_result;
        }

        LOG_I("55x: Host and controller now run at %u", baudrate);
        return CYBT_SUCCESS;
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

    /* Detect a UPDATE_BAUDRATE (0xFC18) command sent by the BT stack, so that
     * the controller rate can be tracked. The controller answers such a
     * command at its old rate and switches afterwards, so the new rate must
     * only be applied to the host once the command complete event arrived -
     * which the stack does via bt_baudrate_updated_cback() ->
     * cybt_platform_hci_set_baudrate(). */
    if ((HCI_PACKET_TYPE_COMMAND == type) && (length >= 10) &&
        (0x01 == p_data[0]) && (0x18 == p_data[1]) && (0xFC == p_data[2]))
    {
        uint32_t new_baud = (uint32_t)p_data[6] |
                            ((uint32_t)p_data[7] << 8) |
                            ((uint32_t)p_data[8] << 16) |
                            ((uint32_t)p_data[9] << 24);

        /* The controller switches to the new rate right after it answered at
         * the old one, so from now on the controller runs at new_baud. */
        s_controller_baud = new_baud;
        LOG_I("55x: Controller baud rate change to %u requested", new_baud);
    }

    /* Detect the LAUNCH_RAM command (0xFC4E). From that point on the
     * controller runs the downloaded application and keeps the baud rate that
     * was used for the download - it does not fall back to the default rate.
     * Infineon's reference tool (mbt.c) states for H1/55x:
     *   "After download success, the controller baudrate will keep in 3M"
     */
    if ((HCI_PACKET_TYPE_COMMAND == type) && (length >= 5) &&
        (0x01 == p_data[0]) && (0x4E == p_data[1]) && (0xFC == p_data[2]))
    {
        s_fw_launched = RT_TRUE;
        LOG_I("55x: Firmware launched, controller stays at %u", s_controller_baud);
    }

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

    (void)type;

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
        if (cybt_result == CYBT_SUCCESS)
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

    LOG_D("55x: HCI transport closed");
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
