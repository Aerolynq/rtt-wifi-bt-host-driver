/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 *                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
 * Website: https://www.aerolynq.com
 *
 * SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
 *
 * PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
 *
 * This file is the proprietary work of Aerolynq and is NOT released
 * under the Apache License or any other open-source license.  Use is
 * permitted EXCLUSIVELY with Aerolynq WiFi / Bluetooth combo modules
 * (e.g. CYWL6208 / CYWL6209 / CYWL6302 / CYWL6305 / CYWL6612).
 * See the NOTICE and LICENSE files at the root of this package for the
 * complete terms.
 *
 * cyhal_uart.h - RT-Thread UART HAL Abstraction for Bluetooth HCI
 *
 * This is a simplified version of cyhal_uart.h adapted for RT-Thread.
 * It wraps RT-Thread Serial Device API to provide the cyhal_uart interface
 * required by BTstack integration layer.
 *
 * Change Logs:
 * Date         Author      Notes
 * 2024-01-11   Aerolynq    first implementation for RT-Thread
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <rtthread.h>
#include <rtdevice.h>
#include "cy_result.h"
#include "cyhal_hw_types.h"
#include "cyhal_modules.h"
#include "cyhal_gpio.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************************************************
*       Defines
*******************************************************************************/

/** Default baud rate if not specified */
#define CYHAL_UART_DEFAULT_BAUD     115200

/** UART error codes */
#define CYHAL_UART_RSLT_ERR_INVALID_PIN \
    (CY_RSLT_CREATE_EX(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_ABSTRACTION_HAL, CYHAL_RSLT_MODULE_UART, 0))

#define CYHAL_UART_RSLT_ERR_INIT_FAILED \
    (CY_RSLT_CREATE_EX(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_ABSTRACTION_HAL, CYHAL_RSLT_MODULE_UART, 1))

#define CYHAL_UART_RSLT_ERR_NOT_SUPPORTED \
    (CY_RSLT_CREATE_EX(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_ABSTRACTION_HAL, CYHAL_RSLT_MODULE_UART, 2))

#define CYHAL_UART_RSLT_ERR_TIMEOUT \
    (CY_RSLT_CREATE_EX(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_ABSTRACTION_HAL, CYHAL_RSLT_MODULE_UART, 3))

#define CYHAL_UART_RSLT_ERR_CONFIG_FAILED \
    (CY_RSLT_CREATE_EX(CY_RSLT_TYPE_ERROR, CY_RSLT_MODULE_ABSTRACTION_HAL, CYHAL_RSLT_MODULE_UART, 4))

/*******************************************************************************
*       Enumerations
*******************************************************************************/

/** UART Parity */
typedef enum
{
    CYHAL_UART_PARITY_NONE = 0, /**< No parity check */
    CYHAL_UART_PARITY_EVEN = 1, /**< Even parity */
    CYHAL_UART_PARITY_ODD  = 2, /**< Odd parity */
} cyhal_uart_parity_t;

/** UART Events for callback */
typedef enum
{
    CYHAL_UART_IRQ_NONE                = 0,
    CYHAL_UART_IRQ_TX_TRANSMIT_IN_FIFO = (1 << 1),
    CYHAL_UART_IRQ_TX_DONE             = (1 << 2),
    CYHAL_UART_IRQ_TX_ERROR            = (1 << 3),
    CYHAL_UART_IRQ_RX_FULL             = (1 << 4),
    CYHAL_UART_IRQ_RX_DONE             = (1 << 5),
    CYHAL_UART_IRQ_RX_ERROR            = (1 << 6),
    CYHAL_UART_IRQ_RX_NOT_EMPTY        = (1 << 7),
    CYHAL_UART_IRQ_TX_EMPTY            = (1 << 8),
} cyhal_uart_event_t;

/*******************************************************************************
*       Typedef
*******************************************************************************/

/** UART configuration structure */
typedef struct
{
    uint32_t data_bits;             /**< Number of data bits (usually 8) */
    uint32_t stop_bits;             /**< Number of stop bits (usually 1) */
    cyhal_uart_parity_t parity;     /**< Parity setting */
    uint8_t *rx_buffer;             /**< Optional RX software buffer */
    uint32_t rx_buffer_size;        /**< Size of RX software buffer */
} cyhal_uart_cfg_t;

/** UART callback function type */
typedef void (*cyhal_uart_event_callback_t)(void *callback_arg, cyhal_uart_event_t event);

/** UART object structure for RT-Thread */
typedef struct
{
    rt_device_t serial;             /**< RT-Thread serial device handle */
    char device_name[RT_NAME_MAX];  /**< Serial device name */
    
    uint32_t baud_rate;             /**< Current baud rate */
    bool flow_control_enabled;      /**< Flow control status */
    
    /* Async operation state */
    volatile bool tx_active;        /**< TX operation in progress */
    volatile bool rx_active;        /**< RX operation in progress */
    
    uint8_t *tx_buffer;             /**< TX async buffer pointer */
    size_t tx_length;               /**< TX async remaining length */
    
    uint8_t *rx_buffer;             /**< RX async buffer pointer */
    size_t rx_length;               /**< RX async total length */
    size_t rx_received;             /**< RX async received count */
    
    /* Callback */
    cyhal_uart_event_callback_t callback;   /**< Event callback */
    void *callback_arg;                      /**< Callback argument */
    cyhal_uart_event_t enabled_events;       /**< Enabled event mask */
    
    /* Synchronization */
    rt_sem_t tx_sem;                /**< TX completion semaphore */
    rt_sem_t rx_sem;                /**< RX completion semaphore */
    rt_mutex_t tx_mutex;            /**< TX atomic mutex */
    rt_mutex_t rx_mutex;            /**< RX atomic mutex */

    /* RX worker (thread-context reads; ISR only signals) */
    volatile bool rx_worker_running; /**< RX worker running flag */
    rt_sem_t rx_notify;             /**< RX notify semaphore */
    rt_thread_t rx_worker;          /**< RX worker thread handle */
    
} cyhal_uart_t;

/*******************************************************************************
*       Functions
*******************************************************************************/

/**
 * Initialize the UART peripheral.
 *
 * @param[out] obj  Pointer to a UART object
 * @param[in]  tx   TX pin (unused in RT-Thread, configured via device)
 * @param[in]  rx   RX pin (unused in RT-Thread, configured via device)
 * @param[in]  cts  CTS pin (enables flow control if not NC)
 * @param[in]  rts  RTS pin (enables flow control if not NC)
 * @param[in]  clk  Clock (unused, set to NULL)
 * @param[in]  cfg  UART configuration
 * @return CY_RSLT_SUCCESS on success
 */
/**
 * Initialize the UART peripheral.
 *
 * @param[out] obj  Pointer to a UART object
 * @param[in]  tx   TX pin (unused in RT-Thread, configured via device)
 * @param[in]  rx   RX pin (unused in RT-Thread, configured via device)
 * @param[in]  cts  CTS pin (enables flow control if not NC)
 * @param[in]  rts  RTS pin (enables flow control if not NC)
 * @param[in]  clk  Clock (unused, set to NULL)
 * @param[in]  cfg  UART configuration
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_init(cyhal_uart_t *obj, cyhal_gpio_t tx, cyhal_gpio_t rx,
                          cyhal_gpio_t cts, cyhal_gpio_t rts,
                          const cyhal_clock_t *clk, const cyhal_uart_cfg_t *cfg);

/**
 * Initialize UART with device name (RT-Thread specific).
 *
 * @param[out] obj          Pointer to a UART object
 * @param[in]  device_name  RT-Thread serial device name (e.g., "uart2")
 * @param[in]  cfg          UART configuration (can be NULL for defaults)
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_init_ex(cyhal_uart_t *obj, const char *device_name,
                              const cyhal_uart_cfg_t *cfg);

/**
 * Release the UART peripheral.
 *
 * @param[in,out] obj  The UART object
 */
void cyhal_uart_free(cyhal_uart_t *obj);

/**
 * Set baud rate.
 *
 * @param[in,out] obj        The UART object
 * @param[in]     baudrate   Desired baud rate
 * @param[out]    actualbaud Actual baud rate achieved (can be NULL)
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_set_baud(cyhal_uart_t *obj, uint32_t baudrate, uint32_t *actualbaud);

/**
 * Configure UART parameters.
 *
 * @param[in,out] obj  The UART object
 * @param[in]     cfg  UART configuration
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_configure(cyhal_uart_t *obj, const cyhal_uart_cfg_t *cfg);

/**
 * Synchronous write.
 *
 * @param[in]     obj        The UART object
 * @param[in]     tx         Data buffer to transmit
 * @param[in,out] tx_length  [in] bytes to send, [out] bytes actually sent
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_write(cyhal_uart_t *obj, void *tx, size_t *tx_length);

/**
 * Synchronous read.
 *
 * @param[in]     obj        The UART object
 * @param[out]    rx         Data buffer to receive into
 * @param[in,out] rx_length  [in] bytes to receive, [out] bytes actually received
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_read(cyhal_uart_t *obj, void *rx, size_t *rx_length);

/**
 * Asynchronous write (non-blocking).
 *
 * @param[in] obj     The UART object
 * @param[in] tx      Data buffer to transmit
 * @param[in] length  Number of bytes to transmit
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_write_async(cyhal_uart_t *obj, void *tx, size_t length);

/**
 * Asynchronous read (non-blocking).
 *
 * @param[in]  obj     The UART object
 * @param[out] rx      Data buffer to receive into
 * @param[in]  length  Number of bytes to receive
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_read_async(cyhal_uart_t *obj, void *rx, size_t length);

/**
 * Check if TX is active.
 *
 * @param[in] obj  The UART object
 * @return true if TX operation is in progress
 */
bool cyhal_uart_is_tx_active(cyhal_uart_t *obj);

/**
 * Check if RX is active.
 *
 * @param[in] obj  The UART object
 * @return true if RX operation is in progress
 */
bool cyhal_uart_is_rx_active(cyhal_uart_t *obj);

/**
 * Abort TX operation.
 *
 * @param[in] obj  The UART object
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_write_abort(cyhal_uart_t *obj);

/**
 * Abort RX operation.
 *
 * @param[in] obj  The UART object
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_read_abort(cyhal_uart_t *obj);

/**
 * Get single character (blocking).
 *
 * @param[in]  obj      The UART object
 * @param[out] value    Received character
 * @param[in]  timeout  Timeout in ms (0 = wait forever)
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_getc(cyhal_uart_t *obj, uint8_t *value, uint32_t timeout);

/**
 * Put single character (blocking).
 *
 * @param[in] obj    The UART object
 * @param[in] value  Character to send
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_putc(cyhal_uart_t *obj, uint32_t value);

/**
 * Get number of bytes available to read.
 *
 * @param[in] obj  The UART object
 * @return Number of bytes available in RX buffer
 */
uint32_t cyhal_uart_readable(cyhal_uart_t *obj);

/**
 * Get number of bytes that can be written.
 *
 * @param[in] obj  The UART object
 * @return Number of bytes that can be written
 */
uint32_t cyhal_uart_writable(cyhal_uart_t *obj);

/**
 * Clear UART buffers.
 *
 * @param[in] obj  The UART object
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_clear(cyhal_uart_t *obj);

/**
 * Enable/disable hardware flow control.
 *
 * @param[in,out] obj         The UART object
 * @param[in]     enable_cts  Enable CTS
 * @param[in]     enable_rts  Enable RTS
 * @return CY_RSLT_SUCCESS on success
 */
cy_rslt_t cyhal_uart_enable_flow_control(cyhal_uart_t *obj, bool enable_cts, bool enable_rts);

/** Alias for cyhal_uart_enable_flow_control (BTstack compatibility) */
#define cyhal_uart_set_flow_control cyhal_uart_enable_flow_control

/**
 * Register event callback.
 *
 * @param[in] obj           The UART object
 * @param[in] callback      Callback function
 * @param[in] callback_arg  Callback argument
 */
void cyhal_uart_register_callback(cyhal_uart_t *obj, cyhal_uart_event_callback_t callback,
                                   void *callback_arg);

/**
 * Enable/disable UART events.
 *
 * @param[in] obj            The UART object
 * @param[in] event          Events to enable/disable
 * @param[in] intr_priority  Interrupt priority (unused in RT-Thread)
 * @param[in] enable         true to enable, false to disable
 */
void cyhal_uart_enable_event(cyhal_uart_t *obj, cyhal_uart_event_t event,
                              uint8_t intr_priority, bool enable);

#if defined(__cplusplus)
}
#endif

