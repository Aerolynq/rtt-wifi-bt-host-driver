/*
 * Copyright 2024-2026, Xinlianxinwei (Hangzhou) Technology Co., Ltd.
 * (新联鑫威（杭州）科技有限公司)
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
 */

/**
 * @file whd_resources_combined.c
 * @brief WHD resource interface implementation
 */

#include <string.h>

/* Enable anonymous unions for ARM Compiler 5 (AC5) only */
#if defined(__CC_ARM)
#pragma anon_unions
#endif

#include "whd_resources_combined.h"

#if !defined(NO_NVRAM_FILE)
#include "wifi_nvram_image.h"
#endif /* NO_NVRAM_FILE */

#include "whd_resource_api.h"
#include "whd_debug.h"
#include "whd.h"

/******************************************************
 *                      Macros
 ******************************************************/
#define BLOCK_BUFFER_SIZE                    (1024)

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/******************************************************
 *                    Constants
 ******************************************************/
#if !defined(NO_NVRAM_FILE)
#if defined(WHD_DYNAMIC_NVRAM)
#define NVRAM_SIZE             dynamic_nvram_size
#define NVRAM_IMAGE_VARIABLE   dynamic_nvram_image
#else
#define NVRAM_SIZE             sizeof(wifi_nvram_image)
#define NVRAM_IMAGE_VARIABLE   wifi_nvram_image
#endif
#endif /* NO_NVRAM_FILE */

/******************************************************
 *               Obfuscation Support
 ******************************************************/

/** Obfuscation key length */
#define WHD_OBFUSCATION_KEY_LEN  16

/** Obfuscation key (must match Python script) */
static const uint8_t whd_obfuscation_key[WHD_OBFUSCATION_KEY_LEN] = {
    0xC7, 0x9A, 0x3E, 0x5B, 0xF1, 0x82, 0x4D, 0xA6,
    0x1F, 0x73, 0xE8, 0x2C, 0x95, 0xD4, 0x6B, 0x0A
};

/**
 * @brief Deobfuscate data in place
 * @param data Buffer to deobfuscate
 * @param len Length of data
 * @param offset Offset from start of payload (for key cycling)
 */
static void whd_deobfuscate(uint8_t *data, uint32_t len, uint32_t offset)
{
    for (uint32_t i = 0; i < len; i++)
    {
        data[i] ^= whd_obfuscation_key[(offset + i) % WHD_OBFUSCATION_KEY_LEN];
    }
}

/******************************************************
 *               Static Variables
 ******************************************************/

/** Buffer for resource block reads */
static unsigned char r_buffer[BLOCK_BUFFER_SIZE];

static const uint8_t *s_combined_data = NULL;
static uint32_t s_combined_size = 0;

/** Cached header info */
static whd_combined_header_t s_cached_header;

/** Header cache valid flag */
static bool s_header_valid = false;

#if defined(WHD_DYNAMIC_NVRAM)
uint32_t dynamic_nvram_size = sizeof(wifi_nvram_image);
void *dynamic_nvram_image = &wifi_nvram_image;
#endif

/******************************************************
 *               Forward Declarations
 ******************************************************/

static whd_result_t combined_resource_init_internal(void);

/******************************************************
 *               Public Functions
 ******************************************************/

bool whd_combined_validate(const uint8_t *combined_data, uint32_t data_size)
{
    const whd_combined_header_t *hdr;
    uint32_t expected_size;

    if (combined_data == NULL || data_size < WHD_COMBINED_HEADER_SIZE)
    {
        return false;
    }

    hdr = WHD_COMBINED_GET_HEADER(combined_data);

    /* Check magic number */
    if (hdr->magic != WHD_COMBINED_MAGIC)
    {
        return false;
    }

    /* Verify checksum */
    if (hdr->checksum != (hdr->magic + hdr->fw_size + hdr->clm_size))
    {
        return false;
    }

    /* Verify size is sufficient */
    expected_size = WHD_COMBINED_HEADER_SIZE +
                    WHD_COMBINED_FW_PADDED_SIZE(hdr->fw_size) +
                    hdr->clm_size;
    if (data_size < expected_size)
    {
        return false;
    }

    return true;
}

const uint8_t *whd_combined_get_firmware(const uint8_t *combined_data, uint32_t *fw_size)
{
    const whd_combined_header_t *hdr;

    if (combined_data == NULL || fw_size == NULL)
    {
        return NULL;
    }

    hdr = WHD_COMBINED_GET_HEADER(combined_data);

    if (!WHD_COMBINED_IS_VALID(hdr))
    {
        return NULL;
    }

    *fw_size = hdr->fw_size;
    return combined_data + WHD_COMBINED_FW_OFFSET;
}

const uint8_t *whd_combined_get_clm(const uint8_t *combined_data, uint32_t *clm_size)
{
    const whd_combined_header_t *hdr;

    if (combined_data == NULL || clm_size == NULL)
    {
        return NULL;
    }

    hdr = WHD_COMBINED_GET_HEADER(combined_data);

    if (!WHD_COMBINED_IS_VALID(hdr))
    {
        return NULL;
    }

    *clm_size = hdr->clm_size;
    return combined_data + WHD_COMBINED_CLM_OFFSET(hdr);
}

whd_result_t whd_combined_get_info(const uint8_t *combined_data,
                                   uint32_t *fw_size,
                                   uint32_t *clm_size)
{
    const whd_combined_header_t *hdr;

    if (combined_data == NULL)
    {
        return WHD_BADARG;
    }

    hdr = WHD_COMBINED_GET_HEADER(combined_data);

    if (!WHD_COMBINED_IS_VALID(hdr))
    {
        return WHD_BADARG;
    }

    if (fw_size != NULL)
    {
        *fw_size = hdr->fw_size;
    }

    if (clm_size != NULL)
    {
        *clm_size = hdr->clm_size;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_combined_resource_init(whd_driver_t whd_drv,
                                        const uint8_t *combined_data,
                                        uint32_t data_size)
{
    const whd_combined_header_t *hdr;

    if (combined_data != NULL)
    {
        s_combined_data = combined_data;
        s_combined_size = data_size;
    }
    else
    {
        s_combined_data = wifi_combined_firmware_data;
        s_combined_size = wifi_combined_firmware_size;
    }

    if (!whd_combined_validate(s_combined_data, s_combined_size))
    {
        WPRINT_WHD_ERROR(("Resource validation failed!\n"));
        s_combined_data = NULL;
        s_combined_size = 0;
        s_header_valid = false;
        return WHD_BADARG;
    }

    hdr = WHD_COMBINED_GET_HEADER(s_combined_data);
    memcpy(&s_cached_header, hdr, sizeof(s_cached_header));
    s_header_valid = true;

    WPRINT_WHD_INFO(("Resource initialized\n"));

    return WHD_SUCCESS;
}

/******************************************************
 *           Resource Interface Implementation
 ******************************************************/

whd_result_t whd_combined_platform_resource_size(whd_driver_t whd_drv,
                                                  whd_resource_type_t resource,
                                                  uint32_t *size_out)
{
    whd_result_t result;

    /* Ensure initialization */
    if (!s_header_valid)
    {
        result = combined_resource_init_internal();
        if (result != WHD_SUCCESS)
        {
            *size_out = 0;
            return result;
        }
    }

    if (resource == WHD_RESOURCE_WLAN_FIRMWARE)
    {
        *size_out = s_cached_header.fw_size;
    }
#if !defined(NO_NVRAM_FILE)
    else if (resource == WHD_RESOURCE_WLAN_NVRAM)
    {
        *size_out = NVRAM_SIZE;
    }
#endif /* NO_NVRAM_FILE */
    else
    {
        *size_out = s_cached_header.clm_size;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_combined_get_resource_block(whd_driver_t whd_drv,
                                              whd_resource_type_t type,
                                              uint32_t blockno,
                                              const uint8_t **data,
                                              uint32_t *size_out)
{
    uint32_t resource_size;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t read_pos;
    const uint8_t *base_ptr = NULL;
    uint32_t remaining;
    whd_result_t result;

    /* Ensure initialization */
    if (!s_header_valid)
    {
        result = combined_resource_init_internal();
        if (result != WHD_SUCCESS)
        {
            return result;
        }
    }

    whd_combined_platform_resource_size(whd_drv, type, &resource_size);
    whd_combined_get_resource_block_size(whd_drv, type, &block_size);
    whd_combined_get_resource_no_of_blocks(whd_drv, type, &block_count);
    
    memset(r_buffer, 0, block_size);
    read_pos = blockno * block_size;

    if (blockno >= block_count)
    {
        return WHD_BADARG;
    }

    if (type == WHD_RESOURCE_WLAN_FIRMWARE)
    {
        base_ptr = s_combined_data + WHD_COMBINED_FW_OFFSET;
    }
#if !defined(NO_NVRAM_FILE)
    else if (type == WHD_RESOURCE_WLAN_NVRAM)
    {
        if (NVRAM_SIZE - read_pos > block_size)
        {
            *size_out = block_size;
        }
        else
        {
            *size_out = NVRAM_SIZE - read_pos;
        }
        *data = ((uint8_t *)NVRAM_IMAGE_VARIABLE) + read_pos;
        return WHD_SUCCESS;
    }
#endif /* NO_NVRAM_FILE */
    else
    {
        base_ptr = s_combined_data + WHD_COMBINED_CLM_OFFSET(&s_cached_header);
    }

    remaining = resource_size - read_pos;
    *size_out = (remaining > block_size) ? block_size : remaining;

    memcpy(r_buffer, base_ptr + read_pos, *size_out);
    
    {
        uint32_t obf_offset;
        if (type == WHD_RESOURCE_WLAN_FIRMWARE)
        {
            obf_offset = read_pos;
        }
        else
        {
            obf_offset = WHD_COMBINED_FW_PADDED_SIZE(s_cached_header.fw_size) + read_pos;
        }
        whd_deobfuscate(r_buffer, *size_out, obf_offset);
    }
    
    *data = (uint8_t *)r_buffer;

    return WHD_SUCCESS;
}

whd_result_t whd_combined_get_resource_block_size(whd_driver_t whd_drv,
                                                   whd_resource_type_t type,
                                                   uint32_t *size_out)
{
    *size_out = BLOCK_BUFFER_SIZE;
    return WHD_SUCCESS;
}

whd_result_t whd_combined_get_resource_no_of_blocks(whd_driver_t whd_drv,
                                                     whd_resource_type_t type,
                                                     uint32_t *block_count)
{
    uint32_t resource_size;
    uint32_t block_size;

    whd_combined_platform_resource_size(whd_drv, type, &resource_size);
    whd_combined_get_resource_block_size(whd_drv, type, &block_size);

    *block_count = resource_size / block_size;
    if (resource_size % block_size)
    {
        *block_count += 1;
    }

    return WHD_SUCCESS;
}

whd_result_t whd_combined_resource_read(whd_driver_t whd_drv,
                                         whd_resource_type_t type,
                                         uint32_t offset,
                                         uint32_t size,
                                         uint32_t *size_out,
                                         void *buffer)
{
    const uint8_t *base_ptr = NULL;
    uint32_t resource_size;
    whd_result_t result;

    /* Ensure initialization */
    if (!s_header_valid)
    {
        result = combined_resource_init_internal();
        if (result != WHD_SUCCESS)
        {
            return result;
        }
    }

    if (type == WHD_RESOURCE_WLAN_FIRMWARE)
    {
        base_ptr = s_combined_data + WHD_COMBINED_FW_OFFSET;
        resource_size = s_cached_header.fw_size;
    }
#if !defined(NO_NVRAM_FILE)
    else if (type == WHD_RESOURCE_WLAN_NVRAM)
    {
        if (size != sizeof(wifi_nvram_image))
        {
            return WHD_BUFFER_SIZE_SET_ERROR;
        }
        memcpy((uint8_t *)buffer, wifi_nvram_image, sizeof(wifi_nvram_image));
        *size_out = sizeof(wifi_nvram_image);
        return WHD_SUCCESS;
    }
#endif /* NO_NVRAM_FILE */
    else
    {
        base_ptr = s_combined_data + WHD_COMBINED_CLM_OFFSET(&s_cached_header);
        resource_size = s_cached_header.clm_size;
    }

    if (offset > resource_size)
    {
        return WHD_BADARG;
    }

    *size_out = MIN(size, resource_size - offset);
    memcpy(buffer, base_ptr + offset, *size_out);

    return WHD_SUCCESS;
}

static whd_resource_source_t combined_resource_ops =
{
    .whd_resource_size         = whd_combined_platform_resource_size,
    .whd_get_resource_block    = whd_combined_get_resource_block,
    .whd_get_resource_no_of_blocks = whd_combined_get_resource_no_of_blocks,
    .whd_get_resource_block_size = whd_combined_get_resource_block_size,
    .whd_resource_read         = whd_combined_resource_read,
};

whd_resource_source_t *whd_combined_get_resource_ops(void)
{
    return &combined_resource_ops;
}

/******************************************************
 *               Static Functions
 ******************************************************/

static whd_result_t combined_resource_init_internal(void)
{
    return whd_combined_resource_init(NULL, NULL, 0);
}

/******************************************************
 *               Standard exports
 ******************************************************/

whd_result_t host_platform_resource_size(whd_driver_t whd_drv,
                                          whd_resource_type_t resource,
                                          uint32_t *size_out)
{
    return whd_combined_platform_resource_size(whd_drv, resource, size_out);
}

whd_result_t host_get_resource_block(whd_driver_t whd_drv,
                                      whd_resource_type_t type,
                                      uint32_t blockno,
                                      const uint8_t **data,
                                      uint32_t *size_out)
{
    return whd_combined_get_resource_block(whd_drv, type, blockno, data, size_out);
}

whd_result_t host_get_resource_block_size(whd_driver_t whd_drv,
                                           whd_resource_type_t type,
                                           uint32_t *size_out)
{
    return whd_combined_get_resource_block_size(whd_drv, type, size_out);
}

whd_result_t host_get_resource_no_of_blocks(whd_driver_t whd_drv,
                                             whd_resource_type_t type,
                                             uint32_t *block_count)
{
    return whd_combined_get_resource_no_of_blocks(whd_drv, type, block_count);
}

whd_result_t host_resource_read(whd_driver_t whd_drv,
                                 whd_resource_type_t type,
                                 uint32_t offset,
                                 uint32_t size,
                                 uint32_t *size_out,
                                 void *buffer)
{
    return whd_combined_resource_read(whd_drv, type, offset, size, size_out, buffer);
}

whd_resource_source_t resource_ops =
{
    .whd_resource_size         = host_platform_resource_size,
    .whd_get_resource_block    = host_get_resource_block,
    .whd_get_resource_no_of_blocks = host_get_resource_no_of_blocks,
    .whd_get_resource_block_size = host_get_resource_block_size,
    .whd_resource_read         = host_resource_read,
};
