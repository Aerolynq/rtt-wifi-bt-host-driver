/*
 * Copyright 2018-2024 Cypress Semiconductor Corporation (an Infineon company)
 * or an affiliate of Cypress Semiconductor Corporation
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
 *
 * This file is derived from Infineon whd-bsp-integration cy_network_buffer_lwip.c
 * with modifications for RT-Thread RTOS.
 *
 * Change Logs:
 * Date         Author          Notes
 * 2025-01-26   Aerolynq        Adapted for RT-Thread with LwIP integration
 */

#include <rtthread.h>
#include "whd.h"
#include "whd_network_types.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"

/* SDIO block size for alignment optimization */
#define SDIO_BLOCK_SIZE (64U)

/*******************************************************************************
 * cy_buffer_pool_init
 ******************************************************************************/
whd_result_t cy_buffer_pool_init(void *tx_packet_pool, void *rx_packet_pool)
{
    RT_UNUSED(tx_packet_pool);
    RT_UNUSED(rx_packet_pool);

    /* Not used for LwIP - LwIP manages its own buffer pools */
    return WHD_SUCCESS;
}

/*******************************************************************************
 * cy_host_buffer_get
 ******************************************************************************/
whd_result_t cy_host_buffer_get(whd_buffer_t *buffer, whd_buffer_dir_t direction,
                                uint16_t size, uint32_t timeout_ms)
{
    struct pbuf *p = NULL;
    uint32_t counter = 0;

    RT_ASSERT(buffer != NULL);

    do
    {
        counter++;

        if (direction == WHD_NETWORK_TX)
        {
            /* For TX: Allocate from POOL if possible to avoid dynamic memory allocation */
            pbuf_type type = (size <= PBUF_POOL_BUFSIZE) ? PBUF_POOL : PBUF_RAM;
            p = pbuf_alloc(PBUF_RAW, size, type);
        }
        else
        {
            /* For RX: Increase allocation size to ensure SDIO can write fully aligned blocks
             * for best throughput performance */
            p = pbuf_alloc(PBUF_RAW, size + SDIO_BLOCK_SIZE, PBUF_RAM);
            if (p != NULL)
            {
                p->len = size;
                p->tot_len -= SDIO_BLOCK_SIZE;
            }
        }

        if (p == NULL)
        {
            rt_thread_mdelay(1);
        }
    } while ((p == NULL) && (counter <= timeout_ms));

    if (p != NULL)
    {
        *buffer = p;
        return WHD_SUCCESS;
    }
    else
    {
        return WHD_BUFFER_ALLOC_FAIL;
    }
}

/*******************************************************************************
 * cy_buffer_release
 ******************************************************************************/
void cy_buffer_release(whd_buffer_t buffer, whd_buffer_dir_t direction)
{
    RT_UNUSED(direction);
    (void)pbuf_free((struct pbuf *)buffer);
}

/*******************************************************************************
 * cy_buffer_get_current_piece_data_pointer
 ******************************************************************************/
uint8_t *cy_buffer_get_current_piece_data_pointer(whd_buffer_t buffer)
{
    RT_ASSERT(buffer != NULL);
    struct pbuf *pbuffer = (struct pbuf *)buffer;
    return (uint8_t *)pbuffer->payload;
}

/*******************************************************************************
 * cy_buffer_get_current_piece_size
 ******************************************************************************/
uint16_t cy_buffer_get_current_piece_size(whd_buffer_t buffer)
{
    RT_ASSERT(buffer != NULL);
    struct pbuf *pbuffer = (struct pbuf *)buffer;
    return (uint16_t)pbuffer->len;
}

/*******************************************************************************
 * cy_buffer_set_size
 ******************************************************************************/
whd_result_t cy_buffer_set_size(whd_buffer_t buffer, uint16_t size)
{
    RT_ASSERT(buffer != NULL);
    struct pbuf *pbuffer = (struct pbuf *)buffer;

    if (size > ((uint16_t)WHD_LINK_MTU +
                LWIP_MEM_ALIGN_SIZE(LWIP_MEM_ALIGN_SIZE(sizeof(struct pbuf))) +
                LWIP_MEM_ALIGN_SIZE(size)))
    {
        return WHD_BUFFER_SIZE_SET_ERROR;
    }

    pbuffer->tot_len = size;
    pbuffer->len = size;

    return WHD_SUCCESS;
}

/*******************************************************************************
 * cy_buffer_add_remove_at_front
 ******************************************************************************/
whd_result_t cy_buffer_add_remove_at_front(whd_buffer_t *buffer, int32_t add_remove_amount)
{
    RT_ASSERT(buffer != NULL);
    struct pbuf **pbuffer = (struct pbuf **)buffer;

    if ((u8_t)0 != pbuf_header(*pbuffer, (s16_t)(-add_remove_amount)))
    {
        return WHD_BUFFER_POINTER_MOVE_ERROR;
    }

    return WHD_SUCCESS;
}

/*******************************************************************************
 * WHD Buffer Operations Structure
 ******************************************************************************/
struct whd_buffer_funcs whd_buffer_ops =
{
    .whd_buffer_add_remove_at_front = cy_buffer_add_remove_at_front,
    .whd_buffer_get_current_piece_data_pointer = cy_buffer_get_current_piece_data_pointer,
    .whd_buffer_get_current_piece_size = cy_buffer_get_current_piece_size,
    .whd_buffer_release = cy_buffer_release,
    .whd_buffer_set_size = cy_buffer_set_size,
    .whd_host_buffer_get = cy_host_buffer_get,
};
