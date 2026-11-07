/*
 * Copyright 2024, Cypress Semiconductor Corporation (an Infineon company)
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

/** @file
 *  Implements WHD WiFi roaming related APIs
 *
 *  This file provides roaming control functions including:
 *  - Roam trigger level (RSSI threshold)
 *  - Roam delta (RSSI improvement needed to roam)
 *  - Roam scan period
 */

/* Enable anonymous unions for Keil ARM Compiler 5 (AC5) */
#if defined(__CC_ARM)
#pragma anon_unions
#endif

#include "whd_chip_constants.h"
#ifndef PROTO_MSGBUF
#include "whd_cdc_bdc.h"
#else
#include "whd_flowring.h"
#endif /* PROTO_MSGBUF */
#include "whd_thread_internal.h"
#include "whd_debug.h"
#include "whd_utils.h"
#include "whd_wifi_api.h"
#include "whd_buffer_api.h"
#include "whd_wlioctl.h"
#include "whd_types.h"
#include "whd_types_int.h"
#include "whd_proto.h"

/******************************************************
*                   Constants
******************************************************/

/******************************************************
*             Static Function Prototypes
******************************************************/

static inline whd_result_t whd_wifi_validate_roam_band(uint32_t band);

/******************************************************
*             Function definitions
******************************************************/

/**
 * Validate roam band parameter
 * WLC_BAND_* are 0..3 (AUTO, 5G, 2G, ALL)
 */
static inline whd_result_t whd_wifi_validate_roam_band(uint32_t band)
{
    return (band <= 3U) ? WHD_SUCCESS : WHD_BADARG;
}

/**
 * Get the current roam trigger level (RSSI threshold)
 *
 * @param ifp           Pointer to whd interface
 * @param roam_trigger  Pointer to store the roam trigger level (dBm)
 * @param band          Band to query (WLC_BAND_2G, WLC_BAND_5G, WLC_BAND_ALL, WLC_BAND_AUTO)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_get_roam_trigger(whd_interface_t ifp, int32_t *roam_trigger, uint32_t band)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    int32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (roam_trigger == NULL)
        return WHD_BADARG;

    if (whd_wifi_validate_roam_band(band) != WHD_SUCCESS)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    /* WLC_GET_ROAM_TRIGGER requires [trigger, band] array, same as Linux bcmdhd driver */
    data = (int32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(int32_t) * 2);
    CHECK_IOCTL_BUFFER(data);
    data[0] = 0;
    data[1] = htod32(band);

    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_ROAM_TRIGGER, buffer, &response));

    data = (int32_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    *roam_trigger = dtoh32(data[0]);

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX));
    return WHD_SUCCESS;
}

/**
 * Set the roam trigger level (RSSI threshold)
 *
 * @param ifp           Pointer to whd interface
 * @param roam_trigger  Roam trigger level in dBm (must be negative, e.g., -70)
 * @param band          Band to set (WLC_BAND_2G, WLC_BAND_5G, WLC_BAND_ALL, WLC_BAND_AUTO)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_set_roam_trigger(whd_interface_t ifp, int32_t roam_trigger, uint32_t band)
{
    whd_buffer_t buffer;
    int32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (whd_wifi_validate_roam_band(band) != WHD_SUCCESS)
        return WHD_BADARG;

    /* Match wl_android.c behavior: trigger must be negative dBm */
    if (roam_trigger >= 0)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    /* WLC_SET_ROAM_TRIGGER requires [trigger_level, band] */
    data = (int32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(int32_t) * 2);
    CHECK_IOCTL_BUFFER(data);
    data[0] = htod32(roam_trigger);
    data[1] = htod32(band);

    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_ROAM_TRIGGER, buffer, 0));
    return WHD_SUCCESS;
}

/**
 * Set roam trigger for all bands (Cypress compatible API)
 *
 * @param ifp           Pointer to whd interface
 * @param trigger_level Roam trigger level in dBm (e.g., -68)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_set_roam_trigger_cy(whd_interface_t ifp, int32_t trigger_level)
{
    whd_result_t result;
    uint32_t roaming_trigger[2];

    roaming_trigger[0] = (uint32_t)trigger_level;
    roaming_trigger[1] = WLC_BAND_ALL;

    result = whd_wifi_set_ioctl_buffer(ifp, WLC_SET_ROAM_TRIGGER, roaming_trigger, sizeof(roaming_trigger));
    if (result != WHD_SUCCESS)
    {
        WPRINT_WHD_ERROR(("Could not set roam trigger\n"));
    }
    else
    {
        WPRINT_WHD_INFO(("Success to set roam trigger, trigger_level = %d\n", (int)trigger_level));
    }

    return result;
}

/**
 * Get the current roam delta (RSSI improvement required to roam)
 *
 * @param ifp           Pointer to whd interface
 * @param roam_delta    Pointer to store the roam delta (dB)
 * @param band          Band to query (WLC_BAND_2G, WLC_BAND_5G, WLC_BAND_ALL, WLC_BAND_AUTO)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_get_roam_delta(whd_interface_t ifp, uint32_t *roam_delta, uint32_t band)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    int32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (roam_delta == NULL)
        return WHD_BADARG;

    if (whd_wifi_validate_roam_band(band) != WHD_SUCCESS)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    /* WLC_GET_ROAM_DELTA requires [delta, band] array, same as Linux bcmdhd driver */
    data = (int32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(int32_t) * 2);
    CHECK_IOCTL_BUFFER(data);
    data[0] = 0;
    data[1] = htod32(band);

    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_ROAM_DELTA, buffer, &response));

    data = (int32_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    *roam_delta = dtoh32(data[0]);

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX));
    return WHD_SUCCESS;
}

/**
 * Set the roam delta (RSSI improvement required to roam)
 *
 * @param ifp           Pointer to whd interface
 * @param roam_delta    Roam delta in dB (e.g., 10 means new AP must be 10dB better)
 * @param band          Band to set (WLC_BAND_2G, WLC_BAND_5G, WLC_BAND_ALL, WLC_BAND_AUTO)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_set_roam_delta(whd_interface_t ifp, uint32_t roam_delta, uint32_t band)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (whd_wifi_validate_roam_band(band) != WHD_SUCCESS)
        return WHD_BADARG;

    /* roam_delta is typically a small dB value (e.g., 5~20). Use a conservative
     * sanity limit to avoid accidentally sending absurd values.
     */
    if (roam_delta > 1000U)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    /* WLC_SET_ROAM_DELTA requires [delta, band] */
    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(uint32_t) * 2);
    CHECK_IOCTL_BUFFER(data);
    data[0] = htod32(roam_delta);
    data[1] = htod32(band);

    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_ROAM_DELTA, buffer, 0));
    return WHD_SUCCESS;
}

/**
 * Get the roam scan period
 *
 * @param ifp               Pointer to whd interface
 * @param roam_scan_period  Pointer to store the roam scan period (seconds)
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_get_roam_scan_period(whd_interface_t ifp, uint32_t *roam_scan_period)
{
    whd_buffer_t buffer;
    whd_buffer_t response;
    uint32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    if (roam_scan_period == NULL)
        return WHD_BADARG;

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(uint32_t));
    CHECK_IOCTL_BUFFER(data);

    CHECK_RETURN(whd_proto_get_ioctl(ifp, WLC_GET_ROAM_SCAN_PERIOD, buffer, &response));

    data = (uint32_t *)whd_buffer_get_current_piece_data_pointer(whd_driver, response);
    CHECK_PACKET_NULL(data, WHD_NO_REGISTER_FUNCTION_POINTER);
    *roam_scan_period = dtoh32(*data);

    CHECK_RETURN(whd_buffer_release(whd_driver, response, WHD_NETWORK_RX));
    return WHD_SUCCESS;
}

/**
 * Set the roam scan period
 *
 * @param ifp               Pointer to whd interface
 * @param roam_scan_period  Roam scan period in seconds
 *
 * @return WHD_SUCCESS or error code
 */
whd_result_t whd_wifi_set_roam_scan_period(whd_interface_t ifp, uint32_t roam_scan_period)
{
    whd_buffer_t buffer;
    uint32_t *data;
    whd_driver_t whd_driver;

    CHECK_IFP_NULL(ifp);

    whd_driver = ifp->whd_driver;
    CHECK_DRIVER_NULL(whd_driver);

    data = (uint32_t *)whd_proto_get_ioctl_buffer(whd_driver, &buffer, sizeof(uint32_t));
    CHECK_IOCTL_BUFFER(data);
    *data = htod32(roam_scan_period);

    CHECK_RETURN(whd_proto_set_ioctl(ifp, WLC_SET_ROAM_SCAN_PERIOD, buffer, 0));
    return WHD_SUCCESS;
}
