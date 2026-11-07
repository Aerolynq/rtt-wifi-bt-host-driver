/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 *                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
 * Website: https://www.aerolynq.com
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
 *
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "rthw.h"

#include "cyhal_gpio.h"
#include "cyhal_sdio.h"
#include "cybsp.h"
#include "whd_bus_sdio_protocol.h"

#ifndef AEROLYNQ_GPIO_SLOT_COUNT
#define AEROLYNQ_GPIO_SLOT_COUNT            (8U)
#endif

/* The host_wake signal may be asserted for a duration shorter than one
 * external interrupt debounce window, in which case the corresponding edge
 * can be missed. A periodic level inspection performed by a software timer
 * recovers from such an event by re-issuing the callback whenever the pin
 * remains asserted longer than this threshold. */
#ifndef AEROLYNQ_GPIO_LATCHED_MS
#define AEROLYNQ_GPIO_LATCHED_MS            (10U)
#endif

typedef struct
{
    cyhal_gpio_t                pin;        /* CYHAL_NC_PIN_VALUE when the slot is unused */
    cyhal_gpio_irq_event_t      event;
    cyhal_gpio_irq_handler_t    handler;
    void                       *handler_arg;
    rt_tick_t                   last_callback_tick;
    rt_uint8_t                  last_level;
    rt_uint8_t                  enabled;
} _gpio_slot_t;

static _gpio_slot_t   _gpio_slots[AEROLYNQ_GPIO_SLOT_COUNT];
static rt_timer_t     _gpio_latch_timer = RT_NULL;

/* Convert the CYHAL drive mode and direction into the RT-Thread PIN_MODE_*
 * representation.
 *
 * The NONE and STRONG drive modes do not depend on the direction argument
 * (NONE maps to input, STRONG maps to output). The pull-up, pull-down and
 * open-drain modes only alter the behaviour when the direction is input; any
 * other direction (output or bidirectional) maps to the output form.
 * PULLUPDOWN is the only mode that promotes the bidirectional direction to
 * the output form.
 */
static rt_uint8_t _resolve_pin_mode(cyhal_gpio_direction_t direction,
                                    cyhal_gpio_drive_mode_t drive)
{
    rt_bool_t is_input_only = (direction == CYHAL_GPIO_DIR_INPUT);

    switch (drive)
    {
    case CYHAL_GPIO_DRIVE_NONE:
        return PIN_MODE_INPUT;

    case CYHAL_GPIO_DRIVE_STRONG:
        return PIN_MODE_OUTPUT;

    case CYHAL_GPIO_DRIVE_PULLUP:
        return is_input_only ? PIN_MODE_INPUT_PULLUP   : PIN_MODE_OUTPUT;

    case CYHAL_GPIO_DRIVE_PULLDOWN:
        return is_input_only ? PIN_MODE_INPUT_PULLDOWN : PIN_MODE_OUTPUT;

    case CYHAL_GPIO_DRIVE_OPENDRAINDRIVESLOW:
        return is_input_only ? PIN_MODE_INPUT_PULLDOWN : PIN_MODE_OUTPUT_OD;

    case CYHAL_GPIO_DRIVE_OPENDRAINDRIVESHIGH:
        return is_input_only ? PIN_MODE_INPUT_PULLUP   : PIN_MODE_OUTPUT_OD;

    case CYHAL_GPIO_DRIVE_PULLUPDOWN:
        return ((direction == CYHAL_GPIO_DIR_OUTPUT) ||
                (direction == CYHAL_GPIO_DIR_BIDIRECTIONAL))
                ? PIN_MODE_OUTPUT : PIN_MODE_INPUT_PULLUP;

    default:
        return PIN_MODE_INPUT;
    }
}

static rt_uint8_t _to_rt_irq_mode(cyhal_gpio_irq_event_t event)
{
    switch (event)
    {
    case CYHAL_GPIO_IRQ_RISE: return PIN_IRQ_MODE_RISING;
    case CYHAL_GPIO_IRQ_FALL: return PIN_IRQ_MODE_FALLING;
    case CYHAL_GPIO_IRQ_BOTH: return PIN_IRQ_MODE_RISING_FALLING;
    default:                  return PIN_IRQ_MODE_RISING_FALLING;
    }
}

static _gpio_slot_t *_gpio_lookup(cyhal_gpio_t pin)
{
    rt_uint32_t i;
    for (i = 0; i < AEROLYNQ_GPIO_SLOT_COUNT; ++i)
    {
        if (_gpio_slots[i].pin == pin && _gpio_slots[i].handler != NULL)
            return &_gpio_slots[i];
    }
    return NULL;
}

static _gpio_slot_t *_gpio_acquire(cyhal_gpio_t pin)
{
    rt_uint32_t i;
    _gpio_slot_t *existing = _gpio_lookup(pin);
    if (existing)
        return existing;

    for (i = 0; i < AEROLYNQ_GPIO_SLOT_COUNT; ++i)
    {
        if (_gpio_slots[i].handler == NULL)
        {
            _gpio_slots[i].pin = pin;
            return &_gpio_slots[i];
        }
    }
    return NULL;
}

static void _gpio_release(_gpio_slot_t *slot)
{
    slot->handler     = NULL;
    slot->handler_arg = NULL;
    slot->enabled     = 0;
    slot->event       = CYHAL_GPIO_IRQ_NONE;
    slot->pin         = CYHAL_NC_PIN_VALUE;
}

static void _gpio_dispatch(void *arg)
{
    _gpio_slot_t *slot = (_gpio_slot_t *)arg;
    if (slot != NULL && slot->enabled && slot->handler != NULL)
    {
        slot->handler(slot->handler_arg, slot->event);
        slot->last_callback_tick = rt_tick_get();
    }
}

/* Supervision of the pin level, used to recover from interrupt edges that
 * were missed by the interrupt controller (see AEROLYNQ_GPIO_LATCHED_MS). */
static void _gpio_poll_tick(void *unused)
{
    rt_uint32_t i;
    rt_tick_t now = rt_tick_get();
    rt_tick_t threshold = rt_tick_from_millisecond(AEROLYNQ_GPIO_LATCHED_MS);

    (void)unused;

    for (i = 0; i < AEROLYNQ_GPIO_SLOT_COUNT; ++i)
    {
        _gpio_slot_t *slot = &_gpio_slots[i];
        rt_uint8_t now_level;
        rt_bool_t latched;

        if (!slot->enabled || slot->handler == NULL)
            continue;

        now_level = rt_pin_read(slot->pin) ? 1U : 0U;

        latched = RT_FALSE;
        if (slot->event == CYHAL_GPIO_IRQ_RISE && slot->last_level && now_level)
            latched = RT_TRUE;
        else if (slot->event == CYHAL_GPIO_IRQ_FALL && !slot->last_level && !now_level)
            latched = RT_TRUE;

        slot->last_level = now_level;

        if (latched && (now - slot->last_callback_tick) >= threshold)
            _gpio_dispatch(slot);
    }
}

static void _gpio_ensure_latch_timer(void)
{
    if (_gpio_latch_timer != RT_NULL)
        return;

    _gpio_latch_timer = rt_timer_create("ay_gpio",
                                        _gpio_poll_tick,
                                        RT_NULL,
                                        rt_tick_from_millisecond(AEROLYNQ_GPIO_LATCHED_MS),
                                        RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (_gpio_latch_timer != RT_NULL)
        rt_timer_start(_gpio_latch_timer);
}

/* =========================================================================
 *  GPIO API
 * ========================================================================= */

cy_rslt_t cyhal_gpio_init(cyhal_gpio_t pin,
                          cyhal_gpio_direction_t direction,
                          cyhal_gpio_drive_mode_t drvMode,
                          bool initVal)
{
    if (pin == CYHAL_NC_PIN_VALUE)
        return CY_RSLT_TYPE_ERROR;

    rt_pin_mode(pin, _resolve_pin_mode(direction, drvMode));
    rt_pin_write(pin, initVal ? PIN_HIGH : PIN_LOW);
    return CY_RSLT_SUCCESS;
}

void cyhal_gpio_free(cyhal_gpio_t pin)
{
    if (pin == CYHAL_NC_PIN_VALUE)
        return;

    rt_pin_detach_irq(pin);
    rt_pin_mode(pin, PIN_MODE_INPUT);
}

void cyhal_gpio_write(cyhal_gpio_t pin, bool value)
{
    if (pin == CYHAL_NC_PIN_VALUE)
        return;
    rt_pin_write(pin, value ? PIN_HIGH : PIN_LOW);
}

bool cyhal_gpio_read(cyhal_gpio_t pin)
{
    if (pin == CYHAL_NC_PIN_VALUE)
        return false;
    return rt_pin_read(pin) != 0;
}

void cyhal_gpio_register_irq(cyhal_gpio_t pin,
                             uint8_t intrPriority,
                             cyhal_gpio_irq_handler_t handler,
                             void *handler_arg)
{
    rt_base_t guard;
    _gpio_slot_t *slot;

    (void)intrPriority;

    if (pin == CYHAL_NC_PIN_VALUE)
        return;

    guard = rt_hw_interrupt_disable();

    if (handler != NULL)
    {
        slot = _gpio_acquire(pin);
        if (slot == NULL)
        {
            rt_hw_interrupt_enable(guard);
            RT_ASSERT(0);   /* The number of GPIO slots has been exhausted */
            return;
        }
        slot->handler     = handler;
        slot->handler_arg = handler_arg;
    }
    else
    {
        slot = _gpio_lookup(pin);
        if (slot != NULL)
            _gpio_release(slot);
    }

    rt_hw_interrupt_enable(guard);
}

void cyhal_gpio_irq_enable(cyhal_gpio_t pin,
                           cyhal_gpio_irq_event_t event,
                           bool enable)
{
    _gpio_slot_t *slot;

    if (pin == CYHAL_NC_PIN_VALUE)
        return;

    slot = _gpio_lookup(pin);
    if (slot == NULL)
    {
        RT_ASSERT(0);   /* The interrupt must be enabled after the handler has been registered */
        return;
    }

    if (enable)
    {
        slot->event              = event;
        slot->last_level         = rt_pin_read(pin) ? 1U : 0U;
        slot->last_callback_tick = rt_tick_get();
        slot->enabled            = 1;

        rt_pin_attach_irq(pin, _to_rt_irq_mode(event), _gpio_dispatch, slot);
        rt_pin_irq_enable(pin, RT_TRUE);

        _gpio_ensure_latch_timer();
    }
    else
    {
        slot->enabled = 0;
        rt_pin_irq_enable(pin, RT_FALSE);
        rt_pin_detach_irq(pin);
    }
}

void cyhal_gpio_register_callback(cyhal_gpio_t pin,
                                  cyhal_gpio_callback_data_t *callback)
{
    if (pin == CYHAL_NC_PIN_VALUE || callback == NULL)
        return;
    cyhal_gpio_register_irq(pin, 0, callback->callback, callback->callback_arg);
}

void cyhal_gpio_enable_event(cyhal_gpio_t pin,
                             cyhal_gpio_event_t event,
                             uint8_t intr_priority,
                             bool enable)
{
    (void)intr_priority;
    cyhal_gpio_irq_enable(pin, event, enable);
}



#ifndef AEROLYNQ_SDIO_BOUNCE_MAX
#define AEROLYNQ_SDIO_BOUNCE_MAX        (512u)
#endif
#ifndef AEROLYNQ_SDIO_BOUNCE_TAIL
#define AEROLYNQ_SDIO_BOUNCE_TAIL       (8u)
#endif

#define AEROLYNQ_SDIO_PROMOTE_BYTE_WRITE_TO_BLOCK   1

#ifndef AEROLYNQ_SDIO_POLL_INTERVAL_MS
#define AEROLYNQ_SDIO_POLL_INTERVAL_MS  2
#endif
#ifndef AEROLYNQ_SDIO_POLL_STACK
#define AEROLYNQ_SDIO_POLL_STACK        512
#endif
#ifndef AEROLYNQ_SDIO_POLL_PRIO
#define AEROLYNQ_SDIO_POLL_PRIO         3
#endif

static cyhal_sdio_t            *_sdio_handle  = NULL;
static cyhal_sdio_irq_handler_t _sdio_irq_cb  = NULL;
static void                    *_sdio_irq_arg = NULL;

static rt_thread_t              _sdio_poll_thread = RT_NULL;
static volatile rt_bool_t       _sdio_poll_run    = RT_FALSE;
static rt_bool_t                _sdio_inband_attached  = RT_FALSE;
static volatile rt_bool_t       _sdio_inband_confirmed = RT_FALSE; /* Set on the first SDIOIT event */
static rt_bool_t                _sdio_poll_banner_done = RT_FALSE; /* The polling notification has been printed */
static rt_bool_t                _sdio_oob_banner_done  = RT_FALSE; /* The OOB notification has been printed */

/* Forward declarations, required by the driver descriptor below. */
static rt_int32_t _sdio_probe_callback(struct rt_mmcsd_card *card);
static rt_int32_t _sdio_remove_callback(struct rt_mmcsd_card *card);

static struct rt_sdio_device_id _sdio_dev_id =
{
    .func_code    = BACKPLANE_FUNCTION,
    .manufacturer = CYHAL_SDIO_MANUFACTURER,
    .product      = CYHAL_SDIO_DRIVER_ID,
};

static struct rt_sdio_driver _sdio_drv =
{
    .name   = CYHAL_SDIO_DRIVER_NAME,
    .probe  = _sdio_probe_callback,
    .remove = _sdio_remove_callback,
    .id     = &_sdio_dev_id,
};

/* Determine whether the BSP provides a usable OOB host_wake pin. */
static rt_bool_t _sdio_has_oob(void)
{
#if defined(CYBSP_USING_PIN_NAME) && defined(CYBSP_HOST_WAKE_IRQ_PIN_NAME)
    return (CYBSP_HOST_WAKE_IRQ_PIN_NAME[0] != '\0') ? RT_TRUE : RT_FALSE;
#elif defined(CYBSP_HOST_WAKE_IRQ_PIN)
    return RT_TRUE;
#else
    return RT_FALSE;
#endif
}

static void _sdio_poll_entry(void *unused)
{
    (void)unused;
    while (_sdio_poll_run)
    {
        rt_thread_mdelay(AEROLYNQ_SDIO_POLL_INTERVAL_MS);

        if (!_sdio_poll_run)
            break;

        /* Report the polling mode when no SDIOIT event has been observed. */
        if (!_sdio_poll_banner_done && !_sdio_inband_confirmed)
        {
            _sdio_poll_banner_done = RT_TRUE;
            rt_kprintf("[cyhal_sdio] card interrupt source: polling (%d ms)\n",
                       AEROLYNQ_SDIO_POLL_INTERVAL_MS);
            rt_kprintf("[cyhal_sdio] SDIOIT is not available on this platform.\n");
            rt_kprintf("[cyhal_sdio] For lower latency, provide the host_wake (OOB)\n");
            rt_kprintf("[cyhal_sdio]   signal or call mmcsd_signal_sdio_irq() in the SDMMC driver ISR.\n");
        }

        if (_sdio_irq_cb != NULL)
            _sdio_irq_cb(_sdio_irq_arg, CYHAL_SDIO_CARD_INTERRUPT);
    }
}

/* RT-Thread SDIO framework callback used by the in-band SDIOIT path. It is
 * invoked from the mmcsd worker thread once the platform SDMMC driver has
 * reported a card interrupt through mmcsd_signal_sdio_irq().
 * On the first invocation the SDIOIT path is confirmed as operational, the
 * polling fallback thread is stopped in order to release CPU and bus
 * resources, and an informational message is printed once. */
static void _sdio_inband_trampoline(struct rt_sdio_function *fn)
{
    (void)fn;

    if (!_sdio_inband_confirmed)
    {
        _sdio_inband_confirmed = RT_TRUE;
        /* Terminate the polling fallback once interrupts are operational. */
        _sdio_poll_run = RT_FALSE;
        rt_kprintf("[cyhal_sdio] card interrupt source: in-band SDIOIT (confirmed)\n");
        rt_kprintf("[cyhal_sdio] The polling fallback has been stopped.\n");
    }

    if (_sdio_irq_cb != NULL)
        _sdio_irq_cb(_sdio_irq_arg, CYHAL_SDIO_CARD_INTERRUPT);
}

static void _sdio_start_polling(void)
{
    if (_sdio_poll_thread == RT_NULL)
    {
        _sdio_poll_run = RT_TRUE;
        _sdio_poll_thread = rt_thread_create("ay_sdio_poll",
                                             _sdio_poll_entry,
                                             RT_NULL,
                                             AEROLYNQ_SDIO_POLL_STACK,
                                             AEROLYNQ_SDIO_POLL_PRIO,
                                             20);
        if (_sdio_poll_thread != RT_NULL)
            rt_thread_startup(_sdio_poll_thread);
    }
    else
    {
        _sdio_poll_run = RT_TRUE;
    }
}

static void _sdio_stop_polling(void)
{
    _sdio_poll_run = RT_FALSE;
}

static void _sdio_arm_irq_source(bool enable)
{
    /* Level 1: the OOB GPIO path is fully handled by the WHD bus_sdio layer
     * through the GPIO adaptation above; no further action is required. */
    if (_sdio_has_oob())
    {
        if (enable && !_sdio_oob_banner_done)
        {
            _sdio_oob_banner_done = RT_TRUE;
            rt_kprintf("[cyhal_sdio] card interrupt source: OOB GPIO\n");
        }
        return;
    }

    if (!enable)
    {
        if (_sdio_inband_attached &&
            _sdio_handle != NULL && _sdio_handle->card != NULL)
        {
            sdio_detach_irq(_sdio_handle->card->sdio_function[_sdio_dev_id.func_code]);
            _sdio_inband_attached = RT_FALSE;
        }
        _sdio_stop_polling();
        return;
    }

    /* Level 2: enable the in-band SDIOIT path through the RT-Thread SDIO
     * framework. This succeeds only when the platform SDMMC driver forwards
     * SDIOIT to mmcsd_signal_sdio_irq(); on platforms that do not, the attach
     * call still reports success while the trampoline is never invoked.
     * For this reason level 3 is always started in parallel as a fallback.
     * Platforms that require a purely interrupt-driven operation must provide
     * the wake pin or complete the SDIOIT path in the SDMMC driver. */
    if (!_sdio_inband_attached &&
        _sdio_handle != NULL && _sdio_handle->card != NULL && _sdio_irq_cb != NULL)
    {
        struct rt_sdio_function *fn =
            _sdio_handle->card->sdio_function[_sdio_dev_id.func_code];

        struct rt_mmcsd_host *host = _sdio_handle->card->host;
        rt_int32_t ret;

        mmcsd_host_lock(host);
        ret = sdio_attach_irq(fn, _sdio_inband_trampoline);
        mmcsd_host_unlock(host);
        if (ret == 0)
        {
            _sdio_inband_attached = RT_TRUE;
            /* sdio_attach_irq() only enables the interrupt on the card side.
             * The SDIOIT mask of the host controller is re-armed by the SDIO
             * interrupt thread after the first interrupt, therefore it is
             * armed once here. */
            if ((host->flags & MMCSD_SUP_SDIO_IRQ) && host->ops->enable_sdio_irq != RT_NULL)
                host->ops->enable_sdio_irq(host, 1);
        }
    }

    /* Level 3: software polling, started in parallel with level 2.
     * When level 2 (SDIOIT) reports the first event, the trampoline clears
     * _sdio_poll_run and the polling thread terminates on its own. */
    _sdio_start_polling();
}

static rt_int32_t _sdio_probe_callback(struct rt_mmcsd_card *card)
{
    if (_sdio_handle != NULL)
    {
        _sdio_handle->card = card;
        if (_sdio_handle->probe != RT_NULL)
            rt_sem_release(_sdio_handle->probe);
    }
    return CYHAL_SDIO_RET_NO_ERRORS;
}

static rt_int32_t _sdio_remove_callback(struct rt_mmcsd_card *card)
{
    (void)card;
    return RT_EOK;
}

/* =========================================================================
 *  SDIO API
 * ========================================================================= */

cy_rslt_t cyhal_sdio_init(cyhal_sdio_t *obj)
{
    if (obj == NULL)
    {
        rt_kprintf("[cyhal_sdio_init] obj is NULL!\n");
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    }

    _sdio_handle  = obj;
    obj->dev_id   = &_sdio_dev_id;

    if (sdio_register_driver(&_sdio_drv) == RT_ENOMEM)
    {
        rt_kprintf("[cyhal_sdio_init] sdio_register_driver returned RT_ENOMEM\n");
        return CYHAL_SDIO_RET_NO_SP_ERRORS;
    }

    return CYHAL_SDIO_RET_NO_ERRORS;
}

void cyhal_sdio_free(cyhal_sdio_t *obj)
{
    (void)obj;
    sdio_unregister_driver(&_sdio_drv);
    _sdio_handle = NULL;
}

cy_rslt_t cyhal_sdio_configure(cyhal_sdio_t *obj, const cyhal_sdio_cfg_t *config)
{
    struct rt_mmcsd_host *host;
    struct rt_sdio_function *fn;

    if (obj == NULL || obj->card == NULL || config == NULL)
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;

    host = obj->card->host;
    fn   = obj->card->sdio_function[obj->dev_id->func_code];

    mmcsd_set_clock(host, config->frequencyhal_hz);
    return sdio_set_block_size(fn, config->block_size);
}

cy_rslt_t cyhal_sdio_send_cmd(const cyhal_sdio_t *obj,
                              cyhal_transfer_t direction,
                              cyhal_sdio_command_t command,
                              uint32_t argument,
                              uint32_t *response)
{
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_host *host;
    cy_rslt_t rc;

    (void)direction;

    if (obj == NULL || obj->card == NULL || obj->card->host == NULL)
        return CYHAL_SDIO_RET_NO_SP_ERRORS;

    host = obj->card->host;

    rt_memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_code = (rt_int32_t)command;
    cmd.arg      = argument;
    cmd.flags    = RESP_SPI_R5 | RESP_R5 | CMD_AC;

    mmcsd_host_lock(host);
    rc = mmcsd_send_cmd(host, &cmd, 0);
    mmcsd_host_unlock(host);

    if (rc != 0)
        return rc;

    /* Decode the SDIO R5 status bits before reporting the result. */
    if (cmd.resp[0] & R5_ERROR)            return CYHAL_SDIO_RET_CMD_IDX_ERROR;
    if (cmd.resp[0] & R5_FUNCTION_NUMBER)  return CYHAL_SDIO_FUNC_NOT_SUPPORTED;
    if (cmd.resp[0] & R5_OUT_OF_RANGE)     return CYHAL_SDIO_RET_RESP_FLAG_ERROR;

    if (response != NULL)
        *response = cmd.resp[0];

    return CYHAL_SDIO_RET_NO_ERRORS;
}

cy_rslt_t cyhal_sdio_bulk_transfer(cyhal_sdio_t *obj,
                                   cyhal_transfer_t direction,
                                   uint32_t argument,
                                   const uint32_t *data,
                                   uint16_t length,
                                   uint32_t *response)
{
    struct rt_mmcsd_req   req;
    struct rt_mmcsd_cmd   cmd;
    struct rt_mmcsd_data  payload;
    sdio_cmd_argument_t  *cmd_arg;
    struct rt_mmcsd_host *host;

    if (obj == NULL || obj->card == NULL || obj->card->host == NULL)
        return CYHAL_SDIO_RET_NO_SP_ERRORS;

    host = obj->card->host;

    rt_memset(&req,     0, sizeof(req));
    rt_memset(&cmd,     0, sizeof(cmd));
    rt_memset(&payload, 0, sizeof(payload));

    cmd.retries  = 1;
    cmd.cmd_code = CYHAL_SDIO_CMD_IO_RW_EXTENDED;
    cmd.arg      = argument;
    cmd.flags    = RESP_SPI_R5 | RESP_R5 | CMD_ADTC;

    cmd_arg = (sdio_cmd_argument_t *)&cmd.arg;

    payload.buf   = (rt_uint32_t *)data;
    payload.flags = (direction == CYHAL_READ) ? DATA_DIR_READ : DATA_DIR_WRITE;

    if (cmd_arg->cmd53.block_mode)
    {
        /* Block mode: the WHD protocol always uses 64-byte blocks. */
        payload.blksize = SDIO_64B_BLOCK;
        payload.blks    = (length + SDIO_64B_BLOCK - 1u) / SDIO_64B_BLOCK;
    }
#if AEROLYNQ_SDIO_PROMOTE_BYTE_WRITE_TO_BLOCK
    else if (direction == CYHAL_WRITE && cmd_arg->cmd53.op_code == 0)
    {
        /* Fixed-address byte write: promoted to a single 64-byte block so
         * that the hardware CRC of the controller is used, which removes the
         * need to handle the software CRC tail. */
        static rt_uint32_t _byte_write_pad[(SDIO_64B_BLOCK + 3u) / 4u];

        cmd_arg->cmd53.block_mode = 1;
        cmd_arg->cmd53.count      = 1;

        rt_memset(_byte_write_pad, 0, sizeof(_byte_write_pad));
        rt_memcpy(_byte_write_pad, data, length);

        payload.buf     = _byte_write_pad;
        payload.blksize = SDIO_64B_BLOCK;
        payload.blks    = 1;
    }
#endif
    else
    {
        /* Stream or byte mode: read operations and incrementing-address
         * write operations. */
        payload.blksize = length;
        payload.blks    = 1;
        payload.flags  |= DATA_STREAM;

        if (direction == CYHAL_WRITE && length <= AEROLYNQ_SDIO_BOUNCE_MAX)
        {
            static rt_uint32_t _bounce_buf[(AEROLYNQ_SDIO_BOUNCE_MAX +
                                            AEROLYNQ_SDIO_BOUNCE_TAIL + 3u) / 4u];
            rt_memcpy(_bounce_buf, data, length);
            payload.buf = _bounce_buf;
        }
    }

    req.cmd  = &cmd;
    req.data = &payload;

    mmcsd_host_lock(host);
    mmcsd_set_data_timeout(&payload, obj->card);
    mmcsd_send_request(host, &req);
    mmcsd_host_unlock(host);

    if (response != NULL)
        *response = cmd.resp[0];

    return (cmd.err != 0) ? CYHAL_SDIO_RET_NO_SP_ERRORS
                          : CYHAL_SDIO_RET_NO_ERRORS;
}

cy_rslt_t cyhal_sdio_transfer_async(cyhal_sdio_t *obj,
                                    cyhal_transfer_t direction,
                                    uint32_t argument,
                                    const uint32_t *data,
                                    uint16_t length)
{
    /* The mmcsd_send_request() interface of RT-Thread is synchronous with
     * respect to the WHD layer, therefore the asynchronous entry point is
     * implemented on top of it in order to preserve the API contract. */
    return cyhal_sdio_bulk_transfer(obj, direction, argument, data, length, NULL);
}

bool cyhal_sdio_is_busy(const cyhal_sdio_t *obj)
{
    (void)obj;
    return false;
}

cy_rslt_t cyhal_sdio_abort_async(const cyhal_sdio_t *obj)
{
    (void)obj;
    return CYHAL_SDIO_RET_NO_ERRORS;
}

void cyhal_sdio_register_irq(cyhal_sdio_t *obj,
                             cyhal_sdio_irq_handler_t handler,
                             void *handler_arg)
{
    (void)obj;
    _sdio_irq_cb  = handler;
    _sdio_irq_arg = handler_arg;
}

void cyhal_sdio_irq_enable(cyhal_sdio_t *obj,
                           cyhal_sdio_irq_event_t event,
                           bool enable)
{
    if (event == CYHAL_SDIO_CARD_INTERRUPT)
        _sdio_arm_irq_source(enable);
}

void cyhal_sdio_register_callback(cyhal_sdio_t *obj,
                                  cyhal_sdio_irq_handler_t handler,
                                  void *handler_arg)
{
    cyhal_sdio_register_irq(obj, handler, handler_arg);
}

void cyhal_sdio_enable_event(cyhal_sdio_t *obj,
                             cyhal_sdio_event_t event,
                             uint8_t priority,
                             bool enable)
{
    (void)priority;
    if (event & CYHAL_SDIO_CARD_INTERRUPT)
        _sdio_arm_irq_source(enable);
}


/* =========================================================================
 *  BSP initialization (cybsp.h implementation)
 * =========================================================================
 *
 * Board Support Package initialization for the Cypress/Infineon WiFi modules.
 * This function configures the power control pin of the WiFi module
 * (WL_REG_ON) and performs the corresponding power cycle during the system
 * startup.
 *
 * Integrated from cybsp.c in order to simplify the build configuration.
 */

static int cybsp_init(void)
{
#if defined(CYBSP_USING_PIN_NAME)
    rt_base_t pin_number = rt_pin_get(CYBSP_REG_ON_PIN_NAME);
#elif defined(CYBSP_REG_ON_PIN)
    rt_base_t pin_number = CYBSP_REG_ON_PIN;
#else
    #error "Please define CYBSP_REG_ON_PIN or CYBSP_REG_ON_PIN_NAME"
#endif
    
    if (pin_number < 0)
    {
        rt_kprintf("[cybsp] ERROR: Invalid REG_ON pin number: %d\n", pin_number);
        return -RT_ERROR;
    }
    
    /* Configure the WL_REG_ON pin. */
    rt_pin_mode(pin_number, PIN_MODE_OUTPUT);

    /* Power cycle the module. */
    rt_pin_write(pin_number, PIN_LOW);
    rt_thread_mdelay(10);   /* Allow the CBUCK regulator to discharge */
    rt_pin_write(pin_number, PIN_HIGH);
    rt_thread_mdelay(250);  /* Wait for the WiFi chip to power up, a longer delay is required for WiFi 6 */
    
    return RT_EOK;
}
INIT_PREV_EXPORT(cybsp_init);
