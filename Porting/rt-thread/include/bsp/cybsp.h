/**
  ******************************************************************************
  * File Name          : cybsp.h
  * Description        : This file provides code for the configuration
  *                      of the cybsp.h instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __CYBSP__H__
#define __CYBSP__H__

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
/* RT-Thread specific: include rtconfig.h to get chip type definitions */
#if defined(__RTTHREAD__) || defined(RT_USING_WIFI)
#include "rtconfig.h"
#include "drv_gpio.h"   /* For GET_PIN macro */

/*
 * Kconfig symbol collision avoidance:
 * The legacy wifi-host-driver-latest package also defines CYBSP_USING_PIN_NAME/NUMBER
 * as choice options. To allow both packages' Kconfigs to be sourced safely, this package
 * uses WHD_BT_USING_PIN_NAME/NUMBER and maps them here for existing code.
 */
#if defined(WHD_BT_USING_PIN_NAME) && !defined(CYBSP_USING_PIN_NAME)
#define CYBSP_USING_PIN_NAME
#endif
#if defined(WHD_BT_USING_PIN_NUMBER) && !defined(CYBSP_USING_PIN_NUMBER)
#define CYBSP_USING_PIN_NUMBER
#endif

/* Map new WBHD_ BSP pin name Kconfig symbols to the legacy CYBSP_ names
 * used throughout the C code.  This keeps the existing C code working
 * after the rename to avoid conflicts with the official wifi-host-driver
 * package. */
#if defined(WBHD_BSP_REG_ON_PIN_NAME) && !defined(CYBSP_REG_ON_PIN_NAME)
#define CYBSP_REG_ON_PIN_NAME WBHD_BSP_REG_ON_PIN_NAME
#endif
#if defined(WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME) && !defined(CYBSP_HOST_WAKE_IRQ_PIN_NAME)
#define CYBSP_HOST_WAKE_IRQ_PIN_NAME WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME
#endif
#endif

/* Platform-specific defines ------------------------------------------------*/
/* Disable XMC7000 data cache functions (not applicable to STM32/GD32)
 * This prevents errors about undeclared function 'Cy_Syslib_IsMemCacheable' */
#ifndef CY_DISABLE_XMC7000_DATA_CACHE
#define CY_DISABLE_XMC7000_DATA_CACHE
#endif

/* Defines ------------------------------------------------------------------*/
#define false 0
#define true 1

/* RT-Thread uses custom malloc implementation in cyabs_rtos_rtthread.c */
#if defined(__RTTHREAD__) || defined(RT_USING_WIFI)
//#define WHD_USE_CUSTOM_MALLOC_IMPL

/* WBHD_BSP_HOST_WAKE_IRQ_EVENT: Infineon WiFi OOB interrupt is fixed to falling edge (active low) */
#define CYBSP_HOST_WAKE_IRQ_EVENT  CYHAL_GPIO_IRQ_FALL
#endif

/* WIFI interface types */
#define CYBSP_SDIO_INTERFACE    (0)
#define CYBSP_SPI_INTERFACE     (1)
#define CYBSP_M2M_INTERFACE     (2)
#define CYBSP_USB_INTERFACE     (3)

/**
	MiddleWare name : Infineon.AIROC-Wi-Fi-Bluetooth-STM32.1.7.1
	MiddleWare fileName : cybsp.h
	MiddleWare version :
*/

/*==========================================================================*/
/* GPIO Pin Definitions - RT-Thread uses GET_PIN() macro                    */
/*==========================================================================*/
#if defined(__RTTHREAD__) || defined(RT_USING_WIFI)

/*--------------------------------------------------------------------------*/
/* Bluetooth UART Pins - Configured via Kconfig (menuconfig)                */
/*--------------------------------------------------------------------------*/
/*
 * BT UART pins (TX/RX/RTS/CTS) are now configured entirely through Kconfig:
 *   - WHD_BT_USING_PIN_NAME + BT_UART_TX_PIN_NAME etc. (string like "PM.9")
 *   - WHD_BT_USING_PIN_NUMBER + BT_UART_TX_PIN etc. (numeric pin number)
 * 
 * BT control pins (REG_ON, DEV_WAKE, HOST_WAKE) are also in Kconfig:
 *   - WHD_BT_USING_PIN_NAME + BT_REG_ON_PIN_NAME etc.
 *   - WHD_BT_USING_PIN_NUMBER + BT_REG_ON_PIN etc.
 * 
 * The ble_app_cfg.c reads these values directly from Kconfig and uses
 * rt_pin_get() to convert pin names to numbers at runtime.
 * 
 * This eliminates the need to modify cybsp.h for different hardware.
 * Just run `menuconfig` and set the pins there.
 */

/* WiFi Control Pins - Use Kconfig configuration at runtime.
 * WiFi driver uses WBHD_BSP_REG_ON_PIN_NAME and WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME
 * from Kconfig with rt_pin_get() at runtime.
 */
#define CYBSP_WIFI_WL_REG_ON   NC
#define CYBSP_WIFI_HOST_WAKE   NC

/* User Button - Not Connected */
#define CYBSP_USER_BTN         NC

#else  /* Non-RT-Thread (STM32 HAL style) */

/*----------   -----------*/
#define CYBSP_BT_UART_CTS      PC0
/*----------   -----------*/
#define CYBSP_BT_UART_TX       PC1
/*----------   -----------*/
#define CYBSP_BT_UART_RX       PC5
/*----------   -----------*/
#define CYBSP_BT_UART_RTS      PC10
/*----------   -----------*/
#define CYBSP_BT_POWER         PC2
/*----------   -----------*/
#define CYBSP_BT_DEVICE_WAKE   PC3
/*----------   -----------*/
#define CYBSP_BT_HOST_WAKE     PC4
/*----------   -----------*/
#define CYBSP_WIFI_WL_REG_ON   PC6
/*----------   -----------*/
#define CYBSP_WIFI_HOST_WAKE   PC7
/*----------   -----------*/
#define CYBSP_USER_BTN         NC

#endif /* __RTTHREAD__ || RT_USING_WIFI */

/*----------   -----------*/
#define CY_WIFI_COUNTRY_CUSTOM      WHD_COUNTRY_XX

/*----------   -----------*/
#define COUNTRY_CUSTOM_ENABLE      false

/*----------   -----------*/
#define COMMAND_CONSOLE_UART_TX      NC

/*----------   -----------*/
#define COMMAND_CONSOLE_UART_RX      NC

/*----------   -----------*/
#define CYHAL_ISR_PRIORITY_DEFAULT      7

/*----------   -----------*/
#define SDMMC_CLK_FREQ_OVERRIDE      0

/*----------   -----------*/
#define CYBSP_WIFI_HOST_WAKE_IRQ_EVENT      CYHAL_GPIO_IRQ_RISE

/*----------   -----------*/
#define CYHAL_UART_MAX_INSTANCES      3

/*----------   -----------*/
//#define CY_WIFI_COUNTRY      WHD_COUNTRY_AUSTRALIA
//#define CY_WIFI_COUNTRY      WHD_COUNTRY_CHINA
// #define CY_WIFI_COUNTRY      WHD_COUNTRY_WORLD_WIDE_XX  /* 使用通用国家码，CLM可能不包含CN */
#define CY_WIFI_COUNTRY      WHD_COUNTRY_UNITED_STATES   /* 55500 FW对 country=XX 可能返回 WHD_WLAN_BADARG，先用US确保上电流程通过 */
//#define CY_WIFI_COUNTRY      WHD_COUNTRY_JAPAN
/*----------   -----------*/
#define _CYHAL_SDIO_DMA_BUFFER_SIZE      0

//rtt add

/* SDIO Chip ID definitions
 * - Broadcom/Legacy chips (CYW43xxx): Manufacturer ID = 0x02D0
 * - Infineon/Cypress chips: Manufacturer ID = 0x04B4
 * 
 * Note: Some CYW43439 (CYWL6209) modules also report 0x04B4 as manufacturer ID
 * with product ID 0xBD3D (Cypress variant), so we need to match that.
 */
#if defined(WHD_USING_CYWL6612) || \
    defined(WHD_USING_CYWL6209)
#define CYHAL_SDIO_MANUFACTURER                         0x04B4  /* Cypress/Infineon */
#else
#define CYHAL_SDIO_MANUFACTURER                         0x02D0  /* Broadcom */
#endif

/* Chip-specific SDIO Driver ID and Name definitions */
/* Support both old (WHD_USING_CHIP_xxx) and new (WHD_USING_CYWLxxxx) macro names */


#if defined(WHD_USING_CYWL6208)
#define CYHAL_SDIO_DRIVER_ID                            0xA9A6
#define CYHAL_SDIO_DRIVER_NAME                          "cywl6208"
#endif

#if defined(WHD_USING_CYWL6305)
#define CYHAL_SDIO_DRIVER_ID                            0xA9BF
#define CYHAL_SDIO_DRIVER_NAME                          "cywl6305"
#endif

#if defined(WHD_USING_CYWL6209)
#define CYHAL_SDIO_DRIVER_ID                            0xBD3D
#define CYHAL_SDIO_DRIVER_NAME                          "cywl6209"
#endif


#if  defined(WHD_USING_CYWL6302)
#define CYHAL_SDIO_DRIVER_ID                            43012
#define CYHAL_SDIO_DRIVER_NAME                          "cywl6302"
#endif


#if defined(WHD_USING_CYWL6612)
#define CYHAL_SDIO_DRIVER_ID                            0xBD3E
#define CYHAL_SDIO_DRIVER_NAME                          "cywl6612"
#endif

/* Legacy chip support */
#if defined(WHD_USING_CHIP_CYW4343W)
#define CYHAL_SDIO_DRIVER_ID                            43430
#define CYHAL_SDIO_DRIVER_NAME                          "cyw4343W"
#endif

#if defined(WHD_USING_CHIP_CYW4373)
#define CYHAL_SDIO_DRIVER_ID                            0x4373
#define CYHAL_SDIO_DRIVER_NAME                          "cyw4373"
#endif

#if defined(WHD_USING_CHIP_CYW43022)
#define CYHAL_SDIO_DRIVER_ID                            43022
#define CYHAL_SDIO_DRIVER_NAME                          "cyw43022"
#endif

#if COUNTRY_CUSTOM_ENABLE == 1
#if defined(CY_WIFI_COUNTRY)
#undef CY_WIFI_COUNTRY
#define CY_WIFI_COUNTRY CY_WIFI_COUNTRY_CUSTOM
#endif
#endif

#if !defined(CYBSP_WIFI_INTERFACE_TYPE)
#define CYBSP_WIFI_INTERFACE_TYPE 	(CYBSP_SDIO_INTERFACE)
#endif

#ifdef __cplusplus
}
#endif
#endif /*__ CYBSP__H_H */

/**
  * @}
  */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
