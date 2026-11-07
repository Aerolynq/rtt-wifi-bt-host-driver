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
 * @file whd_resources_combined_external.c
 * @brief WHD resource interface for external storage (FAL partition or file system)
 *
 * The combined firmware image (FW + CLM) is read from external storage.
 * Two backends are supported:
 *
 *   - WBHD_RESOURCES_IN_EXTERNAL_STORAGE       FAL partition (external flash)
 *   - WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS    file system (SD card, USB disk)
 *
 * Both backends read the very same file format: the combined image with its
 * 16 byte header (magic / fw_size / clm_size / checksum), the firmware part
 * padded to a 512 byte boundary and the CLM blob behind it. The payload is
 * XOR obfuscated with a fixed key.
 *
 * The NVRAM is not read from external storage: it stays compiled into the
 * firmware image. It is only a few hundred bytes, so moving it out would not
 * save anything, while the MAC address handling would become much more
 * involved.
 */

#include <string.h>
#include <stdlib.h>

#include "rtthread.h"

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE) || defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)

#include "whd_resources_combined.h"

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
#include "fal.h"
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
#include <dfs_file.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#if !defined(NO_NVRAM_FILE)
#include "wifi_nvram_image.h"
#endif /* NO_NVRAM_FILE */

#include "whd_resource_api.h"
#include "whd_debug.h"
#include "whd.h"

/******************************************************
 *                      Macros
 ******************************************************/
#ifndef WHD_COMBINED_PARTITION_NAME
#define WHD_COMBINED_PARTITION_NAME    "whd_combined"
#endif

#ifndef WBHD_RESOURCES_FILE_PATH
#if defined(WHD_USING_CYWL6209)
#define WBHD_RESOURCES_FILE_PATH       "/sdcard/whd/wifi_cywl6209_firmware.bin"
#elif defined(WHD_USING_CYWL6302)
#define WBHD_RESOURCES_FILE_PATH       "/sdcard/whd/wifi_cywl6302_firmware.bin"
#elif defined(WHD_USING_CYWL6305)
#define WBHD_RESOURCES_FILE_PATH       "/sdcard/whd/wifi_cywl6305_firmware.bin"
#elif defined(WHD_USING_CYWL6612)
#define WBHD_RESOURCES_FILE_PATH       "/sdcard/whd/wifi_cywl6612_firmware.bin"
#else
#define WBHD_RESOURCES_FILE_PATH       "/sdcard/whd/wifi_cywl6208_firmware.bin"
#endif
#endif

#ifndef WBHD_RESOURCES_BLOCK_SIZE
#define WBHD_RESOURCES_BLOCK_SIZE       (1024)
#endif

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

typedef struct {
    uint32_t combined_size;
} whd_combined_storage_header_t;

#define STORAGE_HEADER_SIZE    sizeof(whd_combined_storage_header_t)

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

static uint8_t *r_buffer = NULL;

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
static const struct fal_partition *s_combined_partition = NULL;
static whd_combined_storage_header_t s_storage_header;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
/** File descriptor of the combined firmware file, kept open between reads. */
static int s_combined_fd = -1;
/** Total size of the combined firmware file. */
static uint32_t s_combined_file_size = 0;
#endif

static whd_combined_header_t s_cached_header;
static bool s_initialized = false;
static bool s_header_valid = false;

#if defined(WHD_DYNAMIC_NVRAM)
uint32_t dynamic_nvram_size = sizeof(wifi_nvram_image);
void *dynamic_nvram_image = &wifi_nvram_image;
#endif

/******************************************************
 *               Static Functions
 ******************************************************/

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)

static whd_result_t read_from_flash(uint32_t offset, uint8_t *buffer, uint32_t size)
{
    if (s_combined_partition == NULL)
    {
        return WHD_BADARG;
    }

    if (fal_partition_read(s_combined_partition, offset, buffer, size) < 0)
    {
        WPRINT_WHD_ERROR(("Failed to read from flash at offset 0x%x, size %u\n",
                          (unsigned int)offset, (unsigned int)size));
        return WHD_WLAN_SDIO_ERROR;
    }

    return WHD_SUCCESS;
}

#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)

/**
 * @brief Read a block of the combined firmware file from the file system
 *
 * The file descriptor is kept open for the whole lifetime of the driver, so
 * that reading the firmware in blocks does not pay the open/close cost for
 * every block.
 */
static whd_result_t read_from_file(uint32_t offset, uint8_t *buffer, uint32_t size)
{
    ssize_t bytes_read;

    if (s_combined_fd < 0)
    {
        return WHD_BADARG;
    }

    if (lseek(s_combined_fd, (off_t)offset, SEEK_SET) == (off_t)-1)
    {
        WPRINT_WHD_ERROR(("Failed to seek to offset 0x%x in '%s'\n",
                          (unsigned int)offset, WBHD_RESOURCES_FILE_PATH));
        return WHD_WLAN_SDIO_ERROR;
    }

    bytes_read = read(s_combined_fd, buffer, size);
    if (bytes_read < 0)
    {
        WPRINT_WHD_ERROR(("Failed to read from '%s' at offset 0x%x, size %u\n",
                          WBHD_RESOURCES_FILE_PATH, (unsigned int)offset, (unsigned int)size));
        return WHD_WLAN_SDIO_ERROR;
    }

    if ((uint32_t)bytes_read != size)
    {
        WPRINT_WHD_ERROR(("Short read from '%s' at offset 0x%x: got %d, expected %u\n",
                          WBHD_RESOURCES_FILE_PATH, (unsigned int)offset,
                          (int)bytes_read, (unsigned int)size));
        return WHD_WLAN_SDIO_ERROR;
    }

    return WHD_SUCCESS;
}

/**
 * @brief Open the combined firmware file and check that it is large enough
 */
static whd_result_t open_combined_file(void)
{
    struct stat file_stat;

    s_combined_fd = open(WBHD_RESOURCES_FILE_PATH, O_RDONLY);
    if (s_combined_fd < 0)
    {
        WPRINT_WHD_ERROR(("Combined firmware file '%s' not found\n", WBHD_RESOURCES_FILE_PATH));
        return WHD_BADARG;
    }

    if (fstat(s_combined_fd, &file_stat) < 0)
    {
        WPRINT_WHD_ERROR(("Failed to stat '%s'\n", WBHD_RESOURCES_FILE_PATH));
        close(s_combined_fd);
        s_combined_fd = -1;
        return WHD_BADARG;
    }

    s_combined_file_size = (uint32_t)file_stat.st_size;

    if (s_combined_file_size < WHD_COMBINED_HEADER_SIZE)
    {
        WPRINT_WHD_ERROR(("Combined firmware file '%s' is too small: %u bytes\n",
                          WBHD_RESOURCES_FILE_PATH, (unsigned int)s_combined_file_size));
        close(s_combined_fd);
        s_combined_fd = -1;
        return WHD_BADARG;
    }

    return WHD_SUCCESS;
}

#endif /* backend selection */

static whd_result_t combined_external_init(void)
{
    whd_result_t result;
    uint8_t header_buf[WHD_COMBINED_HEADER_SIZE];
    whd_combined_header_t *hdr;

    if (s_initialized)
    {
        return s_header_valid ? WHD_SUCCESS : WHD_BADARG;
    }

    /* Allocate read buffer */
    if (r_buffer == NULL)
    {
        r_buffer = rt_malloc(WBHD_RESOURCES_BLOCK_SIZE);
        if (r_buffer == NULL)
        {
            WPRINT_WHD_ERROR(("Failed to allocate read buffer\n"));
            return WHD_MALLOC_FAILURE;
        }
    }

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
    /* Find FAL partition */
    s_combined_partition = fal_partition_find(WHD_COMBINED_PARTITION_NAME);
    if (s_combined_partition == NULL)
    {
        WPRINT_WHD_ERROR(("FAL partition '%s' not found\n", WHD_COMBINED_PARTITION_NAME));
        s_initialized = true;
        s_header_valid = false;
        return WHD_BADARG;
    }

    result = read_from_flash(0, (uint8_t *)&s_storage_header, STORAGE_HEADER_SIZE);
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Failed to read storage header\n"));
        s_initialized = true;
        s_header_valid = false;
        return result;
    }

    if (s_storage_header.combined_size == 0 ||
        s_storage_header.combined_size == 0xFFFFFFFF ||
        s_storage_header.combined_size < WHD_COMBINED_HEADER_SIZE)
    {
        WPRINT_WHD_ERROR(("Invalid storage header: combined_size=%u\n",
                          (unsigned int)s_storage_header.combined_size));
        s_initialized = true;
        s_header_valid = false;
        return WHD_BADARG;
    }

    result = read_from_flash(STORAGE_HEADER_SIZE, header_buf, WHD_COMBINED_HEADER_SIZE);
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
    result = open_combined_file();
    if (result != WHD_SUCCESS)
    {
        s_initialized = true;
        s_header_valid = false;
        return result;
    }

    result = read_from_file(0, header_buf, WHD_COMBINED_HEADER_SIZE);
#endif

    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Failed to read header\n"));
        s_initialized = true;
        s_header_valid = false;
        return result;
    }

    hdr = (whd_combined_header_t *)header_buf;

    if (hdr->magic != WHD_COMBINED_MAGIC)
    {
        WPRINT_WHD_ERROR(("Invalid magic: 0x%08X (expected 0x%08X)\n",
                          (unsigned int)hdr->magic, (unsigned int)WHD_COMBINED_MAGIC));
        s_initialized = true;
        s_header_valid = false;
        return WHD_BADARG;
    }

    if (hdr->checksum != (hdr->magic + hdr->fw_size + hdr->clm_size))
    {
        WPRINT_WHD_ERROR(("Checksum mismatch\n"));
        s_initialized = true;
        s_header_valid = false;
        return WHD_BADARG;
    }

    /* The stored image must be large enough to hold the header, the padded
     * firmware and the CLM blob. */
    {
        uint32_t required = WHD_COMBINED_HEADER_SIZE +
                            WHD_COMBINED_FW_PADDED_SIZE(hdr->fw_size) +
                            hdr->clm_size;
        uint32_t available;

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
        available = s_storage_header.combined_size;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
        available = s_combined_file_size;
#endif

        if (available < required)
        {
            WPRINT_WHD_ERROR(("Combined firmware is truncated: have %u bytes, need %u\n",
                              (unsigned int)available, (unsigned int)required));
            s_initialized = true;
            s_header_valid = false;
            return WHD_BADARG;
        }
    }

    memcpy(&s_cached_header, hdr, sizeof(s_cached_header));
    s_initialized = true;
    s_header_valid = true;

    WPRINT_WHD_INFO(("Resource initialized\n"));

    return WHD_SUCCESS;
}

/******************************************************
 *           Resource Interface Implementation
 ******************************************************/

whd_result_t host_platform_resource_size(whd_driver_t whd_drv,
                                          whd_resource_type_t resource,
                                          uint32_t *size_out)
{
    whd_result_t result;

    result = combined_external_init();
    if (result != WHD_SUCCESS && resource != WHD_RESOURCE_WLAN_NVRAM)
    {
        *size_out = 0;
        return result;
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

whd_result_t host_get_resource_block(whd_driver_t whd_drv,
                                      whd_resource_type_t type,
                                      uint32_t blockno,
                                      const uint8_t **data,
                                      uint32_t *size_out)
{
    uint32_t resource_size = 0;
    uint32_t read_pos;
    uint32_t storage_offset;
    uint32_t remaining;
    whd_result_t result;

    result = combined_external_init();
    if (result != WHD_SUCCESS && type != WHD_RESOURCE_WLAN_NVRAM)
    {
        return result;
    }

    host_platform_resource_size(whd_drv, type, &resource_size);
    read_pos = blockno * WBHD_RESOURCES_BLOCK_SIZE;

    if (read_pos >= resource_size)
    {
        return WHD_BADARG;
    }

#if !defined(NO_NVRAM_FILE)
    if (type == WHD_RESOURCE_WLAN_NVRAM)
    {
        remaining = resource_size - read_pos;
        *size_out = (remaining > WBHD_RESOURCES_BLOCK_SIZE) ? WBHD_RESOURCES_BLOCK_SIZE : remaining;
        *data = ((uint8_t *)NVRAM_IMAGE_VARIABLE) + read_pos;
        return WHD_SUCCESS;
    }
#endif /* NO_NVRAM_FILE */

    if (type == WHD_RESOURCE_WLAN_FIRMWARE)
    {
#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
        storage_offset = STORAGE_HEADER_SIZE + WHD_COMBINED_FW_OFFSET + read_pos;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
        storage_offset = WHD_COMBINED_FW_OFFSET + read_pos;
#endif
    }
    else
    {
#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
        storage_offset = STORAGE_HEADER_SIZE + WHD_COMBINED_CLM_OFFSET(&s_cached_header) + read_pos;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
        storage_offset = WHD_COMBINED_CLM_OFFSET(&s_cached_header) + read_pos;
#endif
    }

    remaining = resource_size - read_pos;
    *size_out = (remaining > WBHD_RESOURCES_BLOCK_SIZE) ? WBHD_RESOURCES_BLOCK_SIZE : remaining;

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
    result = read_from_flash(storage_offset, r_buffer, *size_out);
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
    result = read_from_file(storage_offset, r_buffer, *size_out);
#endif
    if (result != WHD_SUCCESS)
    {
        return result;
    }

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

    *data = r_buffer;
    return WHD_SUCCESS;
}

whd_result_t host_get_resource_no_of_blocks(whd_driver_t whd_drv,
                                             whd_resource_type_t type,
                                             uint32_t *block_count)
{
    uint32_t resource_size = 0;
    
    host_platform_resource_size(whd_drv, type, &resource_size);
    *block_count = (resource_size + WBHD_RESOURCES_BLOCK_SIZE - 1) / WBHD_RESOURCES_BLOCK_SIZE;

    return WHD_SUCCESS;
}

whd_result_t host_get_resource_block_size(whd_driver_t whd_drv,
                                           whd_resource_type_t type,
                                           uint32_t *size_out)
{
    *size_out = WBHD_RESOURCES_BLOCK_SIZE;
    return WHD_SUCCESS;
}

whd_result_t host_resource_read(whd_driver_t whd_drv,
                                 whd_resource_type_t type,
                                 uint32_t offset,
                                 uint32_t size,
                                 uint32_t *size_out,
                                 void *buffer)
{
    uint32_t resource_size = 0;
    uint32_t storage_offset;
    whd_result_t result;

    result = combined_external_init();
    if (result != WHD_SUCCESS && type != WHD_RESOURCE_WLAN_NVRAM)
    {
        return result;
    }

    host_platform_resource_size(whd_drv, type, &resource_size);

    if (offset >= resource_size)
    {
        *size_out = 0;
        return WHD_BADARG;
    }

#if !defined(NO_NVRAM_FILE)
    if (type == WHD_RESOURCE_WLAN_NVRAM)
    {
        uint32_t remaining = resource_size - offset;
        *size_out = (remaining > size) ? size : remaining;
        memcpy(buffer, ((uint8_t *)NVRAM_IMAGE_VARIABLE) + offset, *size_out);
        return WHD_SUCCESS;
    }
#endif /* NO_NVRAM_FILE */

    if (type == WHD_RESOURCE_WLAN_FIRMWARE)
    {
#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
        storage_offset = STORAGE_HEADER_SIZE + WHD_COMBINED_FW_OFFSET + offset;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
        storage_offset = WHD_COMBINED_FW_OFFSET + offset;
#endif
    }
    else
    {
#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
        storage_offset = STORAGE_HEADER_SIZE + WHD_COMBINED_CLM_OFFSET(&s_cached_header) + offset;
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
        storage_offset = WHD_COMBINED_CLM_OFFSET(&s_cached_header) + offset;
#endif
    }

    uint32_t remaining = resource_size - offset;
    *size_out = (remaining > size) ? size : remaining;

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE)
    result = read_from_flash(storage_offset, (uint8_t *)buffer, *size_out);
#elif defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
    result = read_from_file(storage_offset, (uint8_t *)buffer, *size_out);
#endif
    if (result != WHD_SUCCESS)
    {
        return result;
    }
    
    {
        uint32_t obf_offset;
        if (type == WHD_RESOURCE_WLAN_FIRMWARE)
        {
            obf_offset = offset;
        }
        else
        {
            obf_offset = WHD_COMBINED_FW_PADDED_SIZE(s_cached_header.fw_size) + offset;
        }
        whd_deobfuscate((uint8_t *)buffer, *size_out, obf_offset);
    }
    
    return WHD_SUCCESS;
}

/******************************************************
 *               Resource Operations Table
 ******************************************************/

whd_resource_source_t resource_ops =
{
    .whd_resource_size         = host_platform_resource_size,
    .whd_get_resource_block    = host_get_resource_block,
    .whd_get_resource_no_of_blocks = host_get_resource_no_of_blocks,
    .whd_get_resource_block_size = host_get_resource_block_size,
    .whd_resource_read         = host_resource_read,
};

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS)
/**
 * @brief Show information about the combined firmware file
 *
 * Reads the header of the combined firmware file and prints its contents.
 * The file is read directly, so the command also works before the WiFi
 * driver has been initialized.
 */
static void whd_combined_fs_info(int argc, char **argv)
{
    uint8_t header_buf[WHD_COMBINED_HEADER_SIZE];
    whd_combined_header_t *hdr;
    struct stat file_stat;
    int fd;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    fd = open(WBHD_RESOURCES_FILE_PATH, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("whd_combined_fs_info: cannot open '%s'\n", WBHD_RESOURCES_FILE_PATH);
        rt_kprintf("  Is the file system mounted? Is the file present?\n");
        return;
    }

    if (fstat(fd, &file_stat) < 0)
    {
        rt_kprintf("whd_combined_fs_info: cannot stat '%s'\n", WBHD_RESOURCES_FILE_PATH);
        close(fd);
        return;
    }

    if (read(fd, header_buf, WHD_COMBINED_HEADER_SIZE) != (ssize_t)WHD_COMBINED_HEADER_SIZE)
    {
        rt_kprintf("whd_combined_fs_info: cannot read the header of '%s'\n", WBHD_RESOURCES_FILE_PATH);
        close(fd);
        return;
    }
    close(fd);

    hdr = (whd_combined_header_t *)header_buf;

    rt_kprintf("Combined firmware file: %s\n", WBHD_RESOURCES_FILE_PATH);
    rt_kprintf("  File size      : %u bytes\n", (unsigned int)file_stat.st_size);
    rt_kprintf("  Magic          : 0x%08X (expected 0x%08X) %s\n",
               (unsigned int)hdr->magic, (unsigned int)WHD_COMBINED_MAGIC,
               (hdr->magic == WHD_COMBINED_MAGIC) ? "OK" : "MISMATCH");
    rt_kprintf("  Firmware size  : %u bytes\n", (unsigned int)hdr->fw_size);
    rt_kprintf("  CLM size       : %u bytes\n", (unsigned int)hdr->clm_size);
    rt_kprintf("  Checksum       : 0x%08X (expected 0x%08X) %s\n",
               (unsigned int)hdr->checksum,
               (unsigned int)(hdr->magic + hdr->fw_size + hdr->clm_size),
               (hdr->checksum == (hdr->magic + hdr->fw_size + hdr->clm_size)) ? "OK" : "MISMATCH");
    rt_kprintf("  FW offset      : %u bytes\n", (unsigned int)WHD_COMBINED_FW_OFFSET);
    rt_kprintf("  CLM offset     : %u bytes\n", (unsigned int)WHD_COMBINED_CLM_OFFSET(hdr));
    rt_kprintf("  Expected size  : %u bytes\n",
               (unsigned int)(WHD_COMBINED_HEADER_SIZE +
                              WHD_COMBINED_FW_PADDED_SIZE(hdr->fw_size) +
                              hdr->clm_size));
}
MSH_CMD_EXPORT(whd_combined_fs_info, Show information about the combined WiFi firmware file);
#endif /* WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS */

#endif /* defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE) || defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE_FS) */
