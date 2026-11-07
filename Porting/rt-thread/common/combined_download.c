/*
 * Copyright (c) 2024-2025 Xinlian Xinwei (Hangzhou) Technology Co., Ltd.
 *                          (新联鑫威（杭州）科技有限公司 / Aerolynq)
 * Website: https://www.aerolynq.com
 *
 * SPDX-License-Identifier: LicenseRef-Aerolynq-Proprietary
 *
 * PROPRIETARY AND CONFIDENTIAL - ALL RIGHTS RESERVED.
 * Use of this file is permitted EXCLUSIVELY with Aerolynq WiFi /
 * Bluetooth combo modules.  Any other use constitutes copyright
 * infringement.  See the NOTICE and LICENSE files at the root of this
 * package for the complete terms.
 *
 * Combined WiFi Firmware Download via Ymodem
 *
 * This module provides a command to download combined firmware
 * to external flash storage. Only one download command is needed instead of
 * separate firmware and CLM downloads.
 *
 * Usage:
 *   whd_combined_download [partition_name]
 *   
 * Example:
 *   whd_combined_download whd_combined
 *
 * The combined firmware file should be generated using:
 *   python combine_firmware.py firmware.bin clm.blob --bin-only combined.bin
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "rtthread.h"

#if defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE) && defined(RT_USING_RYM)

#include "ymodem.h"
#include "fal.h"
#include "whd_resources_combined.h"

#if defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5)
#include "tiny_md5.h"
#endif /* defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5) */

/******************************************************
 *                      Macros
 ******************************************************/
#ifndef WHD_COMBINED_PARTITION_NAME
#define WHD_COMBINED_PARTITION_NAME    "whd_combined"
#endif

#ifndef WBHD_RESOURCES_BLOCK_SIZE
#define WBHD_RESOURCES_BLOCK_SIZE       (1024)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/**
 * Storage header for combined firmware in external flash
 * This header is written at the beginning of the partition
 */
typedef struct {
    uint32_t combined_size;     /* Size of the combined firmware data */
} whd_combined_storage_header_t;

#define STORAGE_HEADER_SIZE    sizeof(whd_combined_storage_header_t)

/******************************************************
 *               Static Variables
 ******************************************************/
static size_t download_file_total_size, download_file_cur_size;
static const struct fal_partition *download_part = RT_NULL;

/******************************************************
 *               Ymodem Callbacks
 ******************************************************/

static enum rym_code ymodem_on_begin(struct rym_ctx *ctx, rt_uint8_t *buf, rt_size_t len)
{
    char *file_name, *file_size;

    /* Parse file name and size from Ymodem packet */
    file_name = (char *)&buf[0];
    file_size = (char *)&buf[rt_strlen(file_name) + 1];
    download_file_total_size = atol(file_size);

    download_file_cur_size = 0;

    /* Validate file size */
    if (download_file_total_size < WHD_COMBINED_HEADER_SIZE)
    {
        rt_kprintf("\nFile too small! Minimum size is %d bytes (header)\n", WHD_COMBINED_HEADER_SIZE);
        return RYM_CODE_CAN;
    }

    /* Check partition size (need space for storage header + combined data) */
    if (download_file_total_size + STORAGE_HEADER_SIZE > download_part->len)
    {
        rt_kprintf("\nFile is too large! File size (%d) + header (%d) > partition size (%d)\n",
                   download_file_total_size, STORAGE_HEADER_SIZE, download_part->len);
        return RYM_CODE_CAN;
    }

    /* Erase partition area */
    if (fal_partition_erase(download_part, 0, STORAGE_HEADER_SIZE + download_file_total_size) < 0)
    {
        rt_kprintf("\nFile download failed! Partition (%s) erase error!\n", download_part->name);
        return RYM_CODE_CAN;
    }

    rt_kprintf("\nReceiving '%s' (%d bytes)...\n", file_name, download_file_total_size);

    return RYM_CODE_ACK;
}

static enum rym_code ymodem_on_data(struct rym_ctx *ctx, rt_uint8_t *buf, rt_size_t len)
{
    uint32_t write_length = MIN(download_file_total_size - download_file_cur_size, len);

    /* Write combined firmware data (after storage header) */
    if (fal_partition_write(download_part, STORAGE_HEADER_SIZE + download_file_cur_size, buf, write_length) < 0)
    {
        rt_kprintf("\nDownload failed! Partition (%s) write data error!\n", download_part->name);
        return RYM_CODE_CAN;
    }

    download_file_cur_size += write_length;

    return RYM_CODE_ACK;
}

/******************************************************
 *               Helper Functions
 ******************************************************/

#if defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5)
static void print_md5_checksum(uint32_t addr, uint32_t size)
{
    uint8_t md5_value[16];
    uint32_t read_count = 0;
    uint8_t *buf = rt_malloc(WBHD_RESOURCES_BLOCK_SIZE);
    tiny_md5_context *ctx = rt_malloc(sizeof(tiny_md5_context));
    RT_ASSERT(buf != NULL && ctx != NULL);

    /* Calculate the md5 checksum */
    tiny_md5_starts(ctx);
    while (read_count < size)
    {
        uint32_t length = MIN(WBHD_RESOURCES_BLOCK_SIZE, size - read_count);
        if (fal_partition_read(download_part, addr + read_count, buf, length) < 0)
        {
            rt_kprintf("Failed to read while verifying file!\n");
            goto __error;
        }
        tiny_md5_update(ctx, buf, length);
        read_count += length;
    }
    tiny_md5_finish(ctx, md5_value);

    /* Print the md5 checksum */
    rt_kprintf("MD5: ");
    for (uint8_t i = 0; i < sizeof(md5_value); i++)
    {
        rt_kprintf("%02X", md5_value[i]);
    }
    rt_kprintf("\n");

__error:
    rt_free(buf);
    rt_free(ctx);
}
#endif /* defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5) */

/**
 * Validate downloaded combined firmware
 */
static int validate_combined_firmware(void)
{
    whd_combined_header_t header;
    uint32_t expected_size;

    /* Read combined header (after storage header) */
    if (fal_partition_read(download_part, STORAGE_HEADER_SIZE, (uint8_t *)&header, sizeof(header)) < 0)
    {
        rt_kprintf("Failed to read combined header for validation!\n");
        return -1;
    }

    /* Validate magic */
    if (header.magic != WHD_COMBINED_MAGIC)
    {
        rt_kprintf("Invalid magic: 0x%08X (expected 0x%08X)\n", header.magic, WHD_COMBINED_MAGIC);
        return -1;
    }

    /* Validate checksum */
    if (header.checksum != (header.magic + header.fw_size + header.clm_size))
    {
        rt_kprintf("Checksum mismatch! checksum=0x%08X, expected=0x%08X\n",
                   header.checksum, header.magic + header.fw_size + header.clm_size);
        return -1;
    }

    /* Validate size */
    expected_size = WHD_COMBINED_HEADER_SIZE + WHD_COMBINED_FW_PADDED_SIZE(header.fw_size) + header.clm_size;
    if (download_file_total_size < expected_size)
    {
        rt_kprintf("Size mismatch! file_size=%d, expected>=%d\n", download_file_total_size, expected_size);
        return -1;
    }

    /* Print info */
    rt_kprintf("\n============================================\n");
    rt_kprintf("Combined Firmware Validation: PASSED\n");
    rt_kprintf("============================================\n");
    rt_kprintf("  Checksum:    0x%08X\n", header.checksum);
    rt_kprintf("  Total Size:  %u bytes\n", download_file_total_size);
    rt_kprintf("============================================\n");

    return 0;
}

/******************************************************
 *               MSH Command
 ******************************************************/

static void whd_combined_download(int argc, char **argv)
{
    struct rym_ctx rctx;
    const char *partition_name = WHD_COMBINED_PARTITION_NAME;

    if (argc >= 2)
    {
        partition_name = argv[1];
    }

    /* Find FAL partition */
    download_part = fal_partition_find(partition_name);
    if (download_part == NULL)
    {
        rt_kprintf("Download failed! Partition (%s) not found!\n", partition_name);
        rt_kprintf("Usage: whd_combined_download [partition_name]\n");
        rt_kprintf("  Default partition: %s\n", WHD_COMBINED_PARTITION_NAME);
        return;
    }

    rt_kprintf("Waiting for combined firmware via Ymodem...\n");

    /* Start Ymodem receive */
    if (rym_recv_on_device(&rctx, rt_console_get_device(), RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX,
                           ymodem_on_begin, ymodem_on_data, NULL, RT_TICK_PER_SECOND) == RT_EOK)
    {
        if (download_file_cur_size == download_file_total_size)
        {
            /* Validate the downloaded firmware */
            if (validate_combined_firmware() == 0)
            {
                /* Write storage header */
                whd_combined_storage_header_t storage_header;
                storage_header.combined_size = download_file_cur_size;

                if (fal_partition_write(download_part, 0, (const uint8_t *)&storage_header, STORAGE_HEADER_SIZE) < 0)
                {
                    rt_kprintf("Download failed! Write storage header error!\n");
                    return;
                }

                rt_kprintf("\nDownload SUCCESS! Total: %u bytes\n", download_file_total_size);

#if defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5)
                rt_kprintf("Calculating MD5...\n");
                print_md5_checksum(STORAGE_HEADER_SIZE, storage_header.combined_size);
#endif /* defined(PKG_USING_TINYCRYPT) && defined(TINY_CRYPT_MD5) */

                rt_kprintf("\nPlease restart the device to use the new firmware.\n");
            }
            else
            {
                rt_kprintf("\nDownload failed! Firmware validation error!\n");
            }
        }
        else
        {
            rt_kprintf("\nDownload failed! Received: %u bytes, Expected: %u bytes\n",
                       download_file_cur_size, download_file_total_size);
        }
    }
    else
    {
        rt_kprintf("\nDownload aborted or failed!\n");
    }
}
MSH_CMD_EXPORT(whd_combined_download, Download combined WiFi firmware to external flash);

/**
 * Display combined firmware info from external flash
 */
static void whd_combined_info(int argc, char **argv)
{
    const char *partition_name = WHD_COMBINED_PARTITION_NAME;
    const struct fal_partition *part;
    whd_combined_storage_header_t storage_header;
    whd_combined_header_t header;

    if (argc >= 2)
    {
        partition_name = argv[1];
    }

    /* Find partition */
    part = fal_partition_find(partition_name);
    if (part == NULL)
    {
        rt_kprintf("Partition (%s) not found!\n", partition_name);
        return;
    }

    /* Read storage header */
    if (fal_partition_read(part, 0, (uint8_t *)&storage_header, sizeof(storage_header)) < 0)
    {
        rt_kprintf("Failed to read storage header!\n");
        return;
    }

    if (storage_header.combined_size == 0 || storage_header.combined_size == 0xFFFFFFFF)
    {
        rt_kprintf("No combined firmware found in partition '%s'\n", partition_name);
        rt_kprintf("Use 'whd_combined_download' to download firmware first.\n");
        return;
    }

    /* Read combined header */
    if (fal_partition_read(part, STORAGE_HEADER_SIZE, (uint8_t *)&header, sizeof(header)) < 0)
    {
        rt_kprintf("Failed to read combined header!\n");
        return;
    }

    rt_kprintf("\n============================================\n");
    rt_kprintf("Combined Firmware Info: %s\n", partition_name);
    rt_kprintf("============================================\n");
    rt_kprintf("Partition size:  %u bytes\n", part->len);
    rt_kprintf("Combined size:   %u bytes\n", storage_header.combined_size);
    rt_kprintf("--------------------------------------------\n");

    if (header.magic == WHD_COMBINED_MAGIC)
    {
        rt_kprintf("Status:          VALID\n");
        rt_kprintf("Checksum:        0x%08X", header.checksum);
        
        if (header.checksum == (header.magic + header.fw_size + header.clm_size))
        {
            rt_kprintf(" (OK)\n");
        }
        else
        {
            rt_kprintf(" (MISMATCH!)\n");
        }
    }
    else
    {
        rt_kprintf("Status:          INVALID\n");
        rt_kprintf("Magic:           0x%08X (expected 0x%08X)\n", header.magic, WHD_COMBINED_MAGIC);
    }
    rt_kprintf("============================================\n");
}
MSH_CMD_EXPORT(whd_combined_info, Display combined WiFi firmware info from external flash);

#endif /* defined(WBHD_RESOURCES_IN_EXTERNAL_STORAGE) && defined(RT_USING_RYM) */
