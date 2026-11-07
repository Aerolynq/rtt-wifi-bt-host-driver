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
 * cyhal_uart.c - RT-Thread UART HAL Implementation for Bluetooth HCI
 *
 * This implements cyhal_uart interface using RT-Thread Serial Device API.
 * It supports hardware flow control required by Bluetooth HCI UART.
 *
 * Change Logs:
 * Date         Author      Notes
 * 2024-01-11   Aerolynq    first implementation for RT-Thread
 */

#include <rtthread.h>
#include <rtdevice.h>
/* rtdevice.h will include the correct serial.h or serial_v2.h based on rtconfig.h */
#include <string.h>
#include "cyhal_uart.h"
#include "cyhal_gpio.h"  /* For CYHAL_NC_PIN_VALUE */

#ifdef WHD_USING_BLUETOOTH

#define DBG_TAG    "cyhal.uart"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/*******************************************************************************
 *                          Internal Functions
 ******************************************************************************/

#define CYHAL_UART_RX_WORKER_STACK_SIZE   2048
#define CYHAL_UART_RX_WORKER_PRIORITY     20
#define CYHAL_UART_RX_WORKER_TICK         10

static void cyhal_uart_rx_worker_entry(void *parameter)
{
    cyhal_uart_t *obj = (cyhal_uart_t *)parameter;
    if (obj == RT_NULL)
        return;

    while (obj->rx_worker_running)
    {
        /* Wait for RX indication or a new read_async request */
        if (obj->rx_notify != RT_NULL)
        {
            rt_sem_take(obj->rx_notify, RT_WAITING_FOREVER);
        }

        if (!obj->rx_worker_running)
            break;

        if (obj->serial == RT_NULL)
            continue;

        /* Try to consume available bytes to satisfy an outstanding async read */
        if (obj->rx_mutex)
            rt_mutex_take(obj->rx_mutex, RT_WAITING_FOREVER);

        if (obj->rx_active && obj->rx_buffer != RT_NULL && obj->rx_received < obj->rx_length)
        {
            while (obj->rx_received < obj->rx_length)
            {
                size_t to_read = obj->rx_length - obj->rx_received;
                rt_size_t read_len = rt_device_read(obj->serial, 0,
                                                   obj->rx_buffer + obj->rx_received,
                                                   to_read);
                if (read_len == 0)
                    break;

                obj->rx_received += read_len;
            }

            if (obj->rx_received >= obj->rx_length)
            {
                obj->rx_active = false;

                if (obj->rx_sem != RT_NULL)
                    rt_sem_release(obj->rx_sem);

                if (obj->callback && (obj->enabled_events & CYHAL_UART_IRQ_RX_DONE))
                {
                    obj->callback(obj->callback_arg, CYHAL_UART_IRQ_RX_DONE);
                }
            }
        }

        if (obj->rx_mutex)
            rt_mutex_release(obj->rx_mutex);
    }
}

/**
 * UART RX indication callback (called from interrupt context)
 */
static rt_err_t cyhal_uart_rx_indicate(rt_device_t dev, rt_size_t size)
{
    cyhal_uart_t *obj = (cyhal_uart_t *)dev->user_data;
    
    if (obj == RT_NULL)
        return RT_EOK;

    (void)size;

    /* Signal worker thread to drain RX into pending async read buffer. */
    if (obj->rx_notify != RT_NULL)
        rt_sem_release(obj->rx_notify);

    /* Notify RX-not-empty event (used by stack to kick RX task from ISR). */
    if (obj->callback && (obj->enabled_events & CYHAL_UART_IRQ_RX_NOT_EMPTY))
        obj->callback(obj->callback_arg, CYHAL_UART_IRQ_RX_NOT_EMPTY);
    
    return RT_EOK;
}

/**
 * UART TX complete callback
 */
static rt_err_t cyhal_uart_tx_complete(rt_device_t dev, void *buffer)
{
    cyhal_uart_t *obj = (cyhal_uart_t *)dev->user_data;
    
    if (obj == RT_NULL)
        return RT_EOK;
    
    if (obj->tx_active)
    {
        obj->tx_active = false;
        
        /* Signal completion */
        if (obj->tx_sem != RT_NULL)
            rt_sem_release(obj->tx_sem);
        
        /* Trigger callback */
        if (obj->callback && (obj->enabled_events & CYHAL_UART_IRQ_TX_DONE))
        {
            obj->callback(obj->callback_arg, CYHAL_UART_IRQ_TX_DONE);
        }
    }
    
    return RT_EOK;
}

/*******************************************************************************
 *                          Public Functions
 ******************************************************************************/

cy_rslt_t cyhal_uart_init(cyhal_uart_t *obj, cyhal_gpio_t tx, cyhal_gpio_t rx,
                          cyhal_gpio_t cts, cyhal_gpio_t rts,
                          const cyhal_clock_t *clk, const cyhal_uart_cfg_t *cfg)
{
    (void)tx;
    (void)rx;
    (void)clk;
    
    /* Use device name from Kconfig (BT_UART_DEVICE_NAME) */
    cy_rslt_t result = cyhal_uart_init_ex(obj, BT_UART_DEVICE_NAME, cfg);
    
    if (result == CY_RSLT_SUCCESS)
    {
        /* Enable flow control if CTS/RTS pins are specified */
        if (cts != CYHAL_NC_PIN_VALUE || rts != CYHAL_NC_PIN_VALUE)
        {
            obj->flow_control_enabled = true;
            cyhal_uart_enable_flow_control(obj, 
                                           (cts != CYHAL_NC_PIN_VALUE),
                                           (rts != CYHAL_NC_PIN_VALUE));
        }
    }
    
    return result;
}

cy_rslt_t cyhal_uart_init_ex(cyhal_uart_t *obj, const char *device_name,
                              const cyhal_uart_cfg_t *cfg)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(device_name != RT_NULL);
    
    rt_memset(obj, 0, sizeof(cyhal_uart_t));
    rt_strncpy(obj->device_name, device_name, RT_NAME_MAX - 1);
    
    /* Find serial device */
    obj->serial = rt_device_find(device_name);
    if (obj->serial == RT_NULL)
    {
        LOG_E("Failed to find device: %s", device_name);
        return CYHAL_UART_RSLT_ERR_INIT_FAILED;
    }
    
    /* Open device in non-blocking RX mode with blocking TX.
     * The underlying driver will automatically use DMA if configured
     * (BSP_UART1_RX_USING_DMA / BSP_UART1_TX_USING_DMA in rtconfig.h).
     * For serial_v2: use RT_DEVICE_FLAG_RX_NON_BLOCKING for non-blocking receive
     */
    rt_err_t ret = rt_device_open(obj->serial, 
                                   RT_DEVICE_FLAG_RX_NON_BLOCKING | RT_DEVICE_FLAG_TX_BLOCKING);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to open device: %s, error=%d", device_name, ret);
        return CYHAL_UART_RSLT_ERR_INIT_FAILED;
    }
    LOG_I("BT UART opened: %s", device_name);
    
    /* Store user data for callbacks */
    obj->serial->user_data = obj;
    
    /* Set RX indication callback */
    rt_device_set_rx_indicate(obj->serial, cyhal_uart_rx_indicate);
    rt_device_set_tx_complete(obj->serial, cyhal_uart_tx_complete);
    
    /* Create synchronization primitives */
    obj->tx_sem = rt_sem_create("bt_tx", 0, RT_IPC_FLAG_FIFO);
    obj->rx_sem = rt_sem_create("bt_rx", 0, RT_IPC_FLAG_FIFO);
    obj->tx_mutex = rt_mutex_create("bt_txm", RT_IPC_FLAG_PRIO);
    obj->rx_mutex = rt_mutex_create("bt_rxm", RT_IPC_FLAG_PRIO);
    obj->rx_notify = rt_sem_create("bt_rxn", 0, RT_IPC_FLAG_FIFO);
    
    if (obj->tx_sem == RT_NULL || obj->rx_sem == RT_NULL ||
        obj->tx_mutex == RT_NULL || obj->rx_mutex == RT_NULL ||
        obj->rx_notify == RT_NULL)
    {
        cyhal_uart_free(obj);
        return CYHAL_UART_RSLT_ERR_INIT_FAILED;
    }

    /* Start RX worker thread (thread-context reads only). */
    obj->rx_worker_running = true;
    obj->rx_worker = rt_thread_create("bt_rxw",
                                      cyhal_uart_rx_worker_entry,
                                      obj,
                                      CYHAL_UART_RX_WORKER_STACK_SIZE,
                                      CYHAL_UART_RX_WORKER_PRIORITY,
                                      CYHAL_UART_RX_WORKER_TICK);
    if (obj->rx_worker == RT_NULL)
    {
        cyhal_uart_free(obj);
        return CYHAL_UART_RSLT_ERR_INIT_FAILED;
    }
    rt_thread_startup(obj->rx_worker);
    
    /* Apply configuration if provided */
    if (cfg != RT_NULL)
    {
        cyhal_uart_configure(obj, cfg);
    }
    
    /* Set default baud rate */
    obj->baud_rate = CYHAL_UART_DEFAULT_BAUD;
    cyhal_uart_set_baud(obj, obj->baud_rate, RT_NULL);
    
    LOG_I("UART initialized: %s", device_name);
    
    return CY_RSLT_SUCCESS;
}

void cyhal_uart_free(cyhal_uart_t *obj)
{
    if (obj == RT_NULL)
        return;

    /* Stop RX worker */
    obj->rx_worker_running = false;
    if (obj->rx_notify != RT_NULL)
        rt_sem_release(obj->rx_notify);
    if (obj->rx_worker != RT_NULL)
    {
        rt_thread_delete(obj->rx_worker);
        obj->rx_worker = RT_NULL;
    }
    
    /* Close device */
    if (obj->serial != RT_NULL)
    {
        rt_device_close(obj->serial);
        obj->serial = RT_NULL;
    }
    
    /* Free sync primitives */
    if (obj->tx_sem != RT_NULL)
    {
        rt_sem_delete(obj->tx_sem);
        obj->tx_sem = RT_NULL;
    }
    if (obj->rx_sem != RT_NULL)
    {
        rt_sem_delete(obj->rx_sem);
        obj->rx_sem = RT_NULL;
    }
    if (obj->tx_mutex != RT_NULL)
    {
        rt_mutex_delete(obj->tx_mutex);
        obj->tx_mutex = RT_NULL;
    }
    if (obj->rx_mutex != RT_NULL)
    {
        rt_mutex_delete(obj->rx_mutex);
        obj->rx_mutex = RT_NULL;
    }

    if (obj->rx_notify != RT_NULL)
    {
        rt_sem_delete(obj->rx_notify);
        obj->rx_notify = RT_NULL;
    }
    
    LOG_I("UART freed: %s", obj->device_name);
}

cy_rslt_t cyhal_uart_set_baud(cyhal_uart_t *obj, uint32_t baudrate, uint32_t *actualbaud)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    
    struct rt_serial_device *serial = (struct rt_serial_device *)obj->serial;
    rt_base_t level;
    
    LOG_I("Setting baud rate: %d -> %d", obj->baud_rate, baudrate);
    
    /* 
     * CRITICAL FIX for baud rate switching:
     * 
     * When changing baud rate, we MUST close and reopen the device because:
     * 1. DMA mode: DMA buffer contains old baud rate data, index gets out of sync
     * 2. Interrupt mode with Serial V2: Ring buffer contains old data
     * 3. Hardware: UART peripheral needs clean state for new baud rate
     * 
     * Without full reset, old data in buffers will be misinterpreted at new baud rate,
     * causing HCI packet parsing failures (unknown type errors).
     * 
     * This approach works for all platforms (GD32, STM32, etc.) regardless of
     * DMA or interrupt mode.
     */
    
    /* Step 1: Abort any pending RX operation */
    level = rt_hw_interrupt_disable();
    if (obj->rx_active)
    {
        obj->rx_active = false;
        obj->rx_buffer = RT_NULL;
        obj->rx_length = 0;
        obj->rx_received = 0;
    }
    rt_hw_interrupt_enable(level);
    
    /* Step 2: Close the serial device to stop DMA/interrupt and clear buffers */
    rt_device_close(obj->serial);
    
    /* Step 3: Update baud rate in config */
    serial->config.baud_rate = baudrate;
    
    /* Step 4: Reopen device - this will reinitialize with clean state */
    rt_err_t ret = rt_device_open(obj->serial, 
                                   RT_DEVICE_FLAG_RX_NON_BLOCKING | RT_DEVICE_FLAG_TX_BLOCKING);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to reopen UART after baud rate change: %d", ret);
        return CYHAL_UART_RSLT_ERR_NOT_SUPPORTED;
    }
    
    /* Step 5: Restore user data and callbacks */
    obj->serial->user_data = obj;
    rt_device_set_rx_indicate(obj->serial, cyhal_uart_rx_indicate);
    rt_device_set_tx_complete(obj->serial, cyhal_uart_tx_complete);
    
    /* Step 6: Delay for hardware to stabilize after baud rate change
     * Higher baud rates may need more time for PLL/clock to stabilize
     */
    rt_thread_mdelay(50);  /* Wait for UART hardware to stabilize */
    
    /* Step 7: Flush any garbage data in RX buffer */
    {
        uint8_t dummy;
        int flushed = 0;
        while (rt_device_read(obj->serial, 0, &dummy, 1) > 0 && flushed < 512)
        {
            flushed++;
        }
        if (flushed > 0)
        {
            LOG_I("Flushed %d bytes after baud rate change", flushed);
        }
    }
    
    obj->baud_rate = baudrate;
    
    if (actualbaud != RT_NULL)
    {
        *actualbaud = baudrate;
    }
    
    LOG_I("Baud rate set to: %d (device reset complete)", baudrate);
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_configure(cyhal_uart_t *obj, const cyhal_uart_cfg_t *cfg)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(cfg != RT_NULL);
    
    struct rt_serial_device *serial = (struct rt_serial_device *)obj->serial;
    
    /* Update data bits */
    serial->config.data_bits = cfg->data_bits;
    
    /* Update stop bits */
    serial->config.stop_bits = cfg->stop_bits;
    
    /* Update parity */
    switch (cfg->parity)
    {
        case CYHAL_UART_PARITY_NONE:
            serial->config.parity = PARITY_NONE;
            break;
        case CYHAL_UART_PARITY_EVEN:
            serial->config.parity = PARITY_EVEN;
            break;
        case CYHAL_UART_PARITY_ODD:
            serial->config.parity = PARITY_ODD;
            break;
        default:
            serial->config.parity = PARITY_NONE;
            break;
    }
    
    /* Call the driver's configure function directly */
    if (serial->ops && serial->ops->configure)
    {
        serial->ops->configure(serial, &serial->config);
    }
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_write(cyhal_uart_t *obj, void *tx, size_t *tx_length)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(tx != RT_NULL);
    RT_ASSERT(tx_length != RT_NULL);
    
    rt_mutex_take(obj->tx_mutex, RT_WAITING_FOREVER);
    
    rt_size_t written = rt_device_write(obj->serial, 0, tx, *tx_length);
    *tx_length = written;
    
    rt_mutex_release(obj->tx_mutex);
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_read(cyhal_uart_t *obj, void *rx, size_t *rx_length)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(rx != RT_NULL);
    RT_ASSERT(rx_length != RT_NULL);
    
    rt_mutex_take(obj->rx_mutex, RT_WAITING_FOREVER);
    
    rt_size_t read_len = rt_device_read(obj->serial, 0, rx, *rx_length);
    *rx_length = read_len;
    
    rt_mutex_release(obj->rx_mutex);
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_write_async(cyhal_uart_t *obj, void *tx, size_t length)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(tx != RT_NULL);
    
    rt_mutex_take(obj->tx_mutex, RT_WAITING_FOREVER);
    
    obj->tx_buffer = (uint8_t *)tx;
    obj->tx_length = length;
    obj->tx_active = true;
    
    /* For RT-Thread, we do blocking write and signal completion */
    rt_size_t written = rt_device_write(obj->serial, 0, tx, length);
    
    obj->tx_active = false;
    
    rt_mutex_release(obj->tx_mutex);
    
    /* Signal completion */
    if (obj->tx_sem != RT_NULL)
        rt_sem_release(obj->tx_sem);
    
    /* Trigger callback */
    if (written == length && obj->callback && (obj->enabled_events & CYHAL_UART_IRQ_TX_DONE))
    {
        obj->callback(obj->callback_arg, CYHAL_UART_IRQ_TX_DONE);
    }
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_read_async(cyhal_uart_t *obj, void *rx, size_t length)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(rx != RT_NULL);
    
    rt_mutex_take(obj->rx_mutex, RT_WAITING_FOREVER);

    obj->rx_buffer = (uint8_t *)rx;
    obj->rx_length = length;
    obj->rx_received = 0;
    obj->rx_active = true;

    rt_mutex_release(obj->rx_mutex);

    /* Kick worker to drain any already-buffered data. */
    if (obj->rx_notify != RT_NULL)
        rt_sem_release(obj->rx_notify);
    
    return CY_RSLT_SUCCESS;
}

bool cyhal_uart_is_tx_active(cyhal_uart_t *obj)
{
    return (obj != RT_NULL) ? obj->tx_active : false;
}

bool cyhal_uart_is_rx_active(cyhal_uart_t *obj)
{
    return (obj != RT_NULL) ? obj->rx_active : false;
}

cy_rslt_t cyhal_uart_write_abort(cyhal_uart_t *obj)
{
    RT_ASSERT(obj != RT_NULL);
    
    obj->tx_active = false;
    obj->tx_buffer = RT_NULL;
    obj->tx_length = 0;
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_read_abort(cyhal_uart_t *obj)
{
    RT_ASSERT(obj != RT_NULL);

    if (obj->rx_mutex)
        rt_mutex_take(obj->rx_mutex, RT_WAITING_FOREVER);

    obj->rx_active = false;
    obj->rx_buffer = RT_NULL;
    obj->rx_length = 0;
    obj->rx_received = 0;

    if (obj->rx_mutex)
        rt_mutex_release(obj->rx_mutex);
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_getc(cyhal_uart_t *obj, uint8_t *value, uint32_t timeout)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    RT_ASSERT(value != RT_NULL);
    
    rt_tick_t start_tick = rt_tick_get();
    rt_tick_t timeout_tick = (timeout == 0) ? (rt_tick_t)RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout);
    
    while (1)
    {
        rt_size_t read_len = rt_device_read(obj->serial, 0, value, 1);
        if (read_len == 1)
        {
            return CY_RSLT_SUCCESS;
        }
        
        if (timeout != 0)
        {
            if ((rt_tick_get() - start_tick) >= timeout_tick)
            {
                return CYHAL_UART_RSLT_ERR_TIMEOUT;
            }
        }
        
        rt_thread_mdelay(1);
    }
}

cy_rslt_t cyhal_uart_putc(cyhal_uart_t *obj, uint32_t value)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    
    uint8_t ch = (uint8_t)value;
    rt_device_write(obj->serial, 0, &ch, 1);
    
    return CY_RSLT_SUCCESS;
}

uint32_t cyhal_uart_readable(cyhal_uart_t *obj)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    
    /* Use RT-Thread device control to query available bytes.
     * This is more portable than accessing internal structures.
     * If control is not supported, try a non-blocking peek read.
     */
    struct rt_serial_device *serial = (struct rt_serial_device *)obj->serial;
    
#if defined(RT_USING_SERIAL_V2)
    /* For serial_v2, access ringbuffer through serial_rx */
    struct rt_serial_rx_fifo *rx_fifo = (struct rt_serial_rx_fifo *)serial->serial_rx;
    if (rx_fifo != RT_NULL)
    {
        return rt_ringbuffer_data_len(&rx_fifo->rb);
    }
#else
    /* For serial v1 or other implementations, use device control */
    rt_size_t rx_len = 0;
    if (rt_device_control(obj->serial, RT_DEVICE_CTRL_GET_READABLE, &rx_len) == RT_EOK)
    {
        return (uint32_t)rx_len;
    }
#endif
    
    return 0;
}

uint32_t cyhal_uart_writable(cyhal_uart_t *obj)
{
    /* Always assume we can write */
    (void)obj;
    return 1;
}

cy_rslt_t cyhal_uart_clear(cyhal_uart_t *obj)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    
    /* Clear RX buffer by reading all available data */
    uint8_t dummy;
    while (rt_device_read(obj->serial, 0, &dummy, 1) > 0)
    {
        /* Discard data */
    }
    
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cyhal_uart_enable_flow_control(cyhal_uart_t *obj, bool enable_cts, bool enable_rts)
{
    RT_ASSERT(obj != RT_NULL);
    RT_ASSERT(obj->serial != RT_NULL);
    
    obj->flow_control_enabled = (enable_cts || enable_rts);
    
    /* Configure hardware flow control via RT-Thread serial driver */
    if (enable_cts || enable_rts)
    {
        /* Get the serial device structure to access current configuration */
        struct rt_serial_device *serial = (struct rt_serial_device *)obj->serial;
        
        /* Update the configuration */
        serial->config.flowcontrol = RT_SERIAL_FLOWCONTROL_CTSRTS;
        
        /* Reconfigure the serial device by calling driver's configure directly
         * Note: rt_device_control(RT_DEVICE_CTRL_CONFIG) may not handle serial_configure
         * correctly in some drivers (e.g., STM32H7RS), so we call configure() directly.
         */
        if (serial->ops && serial->ops->configure)
        {
            rt_err_t ret = serial->ops->configure(serial, &serial->config);
            if (ret != RT_EOK)
            {
                LOG_E("Failed to enable flow control: %d", ret);
                return CYHAL_UART_RSLT_ERR_CONFIG_FAILED;
            }
        }
        
        LOG_I("Hardware flow control enabled (CTS=%d, RTS=%d)", enable_cts, enable_rts);
    }
    else
    {
        LOG_D("Flow control disabled");
    }
    
    return CY_RSLT_SUCCESS;
}

void cyhal_uart_register_callback(cyhal_uart_t *obj, cyhal_uart_event_callback_t callback,
                                   void *callback_arg)
{
    RT_ASSERT(obj != RT_NULL);
    
    obj->callback = callback;
    obj->callback_arg = callback_arg;
}

void cyhal_uart_enable_event(cyhal_uart_t *obj, cyhal_uart_event_t event,
                              uint8_t intr_priority, bool enable)
{
    RT_ASSERT(obj != RT_NULL);
    (void)intr_priority;
    
    if (enable)
    {
        obj->enabled_events |= event;
    }
    else
    {
        obj->enabled_events &= ~event;
    }
}

#endif /* WHD_USING_BLUETOOTH */
