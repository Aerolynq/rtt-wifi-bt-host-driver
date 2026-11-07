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
 * @file whd_resources_combined.h
 * @brief WHD resource interface
 */

#ifndef WHD_RESOURCES_COMBINED_H
#define WHD_RESOURCES_COMBINED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "whd.h"
#include "whd_resource_api.h"

/******************************************************
 *                    Constants
 ******************************************************/

#define WHD_COMBINED_MAGIC              0x57494649
#define WHD_COMBINED_VERSION            1
#define WHD_COMBINED_FW_ALIGNMENT       512
#define WHD_COMBINED_HEADER_SIZE        16

/******************************************************
 *                 Type Definitions
 ******************************************************/

typedef struct {
    uint32_t magic;
    uint32_t fw_size;
    uint32_t clm_size;
    uint32_t checksum;
} whd_combined_header_t;

#define WHD_COMBINED_GET_HEADER(data) \
    ((const whd_combined_header_t *)(data))

#define WHD_COMBINED_IS_VALID(hdr) \
    ((hdr)->magic == WHD_COMBINED_MAGIC && \
     (hdr)->checksum == ((hdr)->magic + (hdr)->fw_size + (hdr)->clm_size))

#define WHD_COMBINED_FW_OFFSET          WHD_COMBINED_HEADER_SIZE

#define WHD_COMBINED_FW_PADDED_SIZE(fw_size) \
    (((fw_size) + (WHD_COMBINED_FW_ALIGNMENT - 1)) & ~(WHD_COMBINED_FW_ALIGNMENT - 1))

#define WHD_COMBINED_CLM_OFFSET(hdr) \
    (WHD_COMBINED_HEADER_SIZE + WHD_COMBINED_FW_PADDED_SIZE((hdr)->fw_size))

extern const uint8_t wifi_combined_firmware_data[];
extern const uint32_t wifi_combined_firmware_size;

whd_result_t whd_combined_resource_init(whd_driver_t whd_drv,
                                        const uint8_t *combined_data,
                                        uint32_t data_size);

bool whd_combined_validate(const uint8_t *combined_data, uint32_t data_size);

const uint8_t *whd_combined_get_firmware(const uint8_t *combined_data, uint32_t *fw_size);

const uint8_t *whd_combined_get_clm(const uint8_t *combined_data, uint32_t *clm_size);

whd_result_t whd_combined_get_info(const uint8_t *combined_data,
                                   uint32_t *fw_size,
                                   uint32_t *clm_size);

whd_result_t whd_combined_platform_resource_size(whd_driver_t whd_drv,
                                                  whd_resource_type_t resource,
                                                  uint32_t *size_out);

whd_result_t whd_combined_get_resource_block(whd_driver_t whd_drv,
                                              whd_resource_type_t type,
                                              uint32_t blockno,
                                              const uint8_t **data,
                                              uint32_t *size_out);

whd_result_t whd_combined_get_resource_block_size(whd_driver_t whd_drv,
                                                   whd_resource_type_t type,
                                                   uint32_t *size_out);

whd_result_t whd_combined_get_resource_no_of_blocks(whd_driver_t whd_drv,
                                                     whd_resource_type_t type,
                                                     uint32_t *block_count);

whd_result_t whd_combined_resource_read(whd_driver_t whd_drv,
                                         whd_resource_type_t type,
                                         uint32_t offset,
                                         uint32_t size,
                                         uint32_t *size_out,
                                         void *buffer);

whd_resource_source_t *whd_combined_get_resource_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* WHD_RESOURCES_COMBINED_H */
