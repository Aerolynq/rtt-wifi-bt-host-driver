/*
 * Copyright (c) 2019, Cypress Semiconductor Corporation, All Rights Reserved
 * SPDX-License-Identifier: LicenseRef-PBL
 *
 * This file and the related binary are licensed under the
 * Permissive Binary License, Version 1.0 (the "License");
 * you may not use these files except in compliance with the License.
 *
 * You may obtain a copy of the License here:
 * LICENSE-permissive-binary-license-1.0.txt and at
 * https://www.mbed.com/licenses/PBL-1.0
 *
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDED_NVRAM_IMAGE_H_
#define INCLUDED_NVRAM_IMAGE_H_

#include <string.h>
#include <stdint.h>
#include "../../generated_mac_address.txt"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Character array of NVRAM image
 * Generated from bcm943457wlsagb_4L_170913_AZ_Joe.txt
 */

static const char wifi_nvram_image[] =
        "NVRAMRev=$Rev: 498373 $"                                            "\x00"
        "sromrev=11"                                                       	 "\x00"
        "vendid=0x14e4"                                                      "\x00"
        "devid=0x43ab"                                                       "\x00"
        "manfid=0x2d0"                                                       "\x00"
        "prodid=0x06e4"                                                      "\x00"
        "macaddr=00:90:4c:c5:12:38"                                          "\x00"
        "nocrc=1"                                                        	 "\x00"
        "boardtype=0x6e4"                                                    "\x00"
        "boardrev=0x1304"                                              		 "\x00"
        "xtalfreq=37400"                                                     "\x00"
        "btc_mode=1"                                                         "\x00"
        "boardflags=0x00480201"                                              "\x00"
        "boardflags2=0x40800000"                                             "\x00"
        "boardflags3=0x48200100"                                             "\x00"
        "phycal_tempdelta=15"                                                "\x00"
        "rxchain=1"  	                                                     "\x00"
        "txchain=1"				                                             "\x00"
        "aa2g=1"			                                                 "\x00"
        "aa5g=1"                                               				 "\x00"
        "tssipos5g=1"                                                    	 "\x00"
        "tssipos2g=1"                                                        "\x00"
        "femctrl=0"                                                       	 "\x00"
        "AvVmid_c0=1,165,2,100,2,100,2,100,2,100"                            "\x00"
        "pa2ga0=-166,6659,-751"                                         	 "\x00"
        "pa2ga1=-118,5285,-661"                                              "\x00"
        "pa5ga0=-157,7297,-846,-157,7368,-849,-156,7436,-853,-152,7382,-847" "\x00"
        "pa5ga1=-142,4968,-659,-150,4973,-656,-156,4937,-650,-158,4901,-646" "\x00"
        "itrsw=1"                                               			 "\x00"
        "pdoffset2g40ma0=15"                                                 "\x00"
        "pdoffset40ma0=0x8888"                                               "\x00"
        "pdoffset80ma0=0x8888"                                               "\x00"
        "extpagain5g=2"                                                  	 "\x00"
        "extpagain2g=2"                                                      "\x00"
        "tworangetssi2g=1"                                                   "\x00"
        "tworangetssi5g=1"                                              	 "\x00"
        "ltecxmux=0"                                                       	 "\x00"
        "ltecxpadnum=0x0504"                                                 "\x00"
		"ltecxfnsel=0x22"                                                 	 "\x00"
		"ltecxgcigpio=0x32"                                                  "\x00"

		"maxp2ga0=78"                                                 		 "\x00"
		"ofdmlrbw202gpo=0x0044"                                              "\x00"
		"dot11agofdmhrbw202gpo=0x4444"                                       "\x00"
		"mcsbw202gpo=0x66666666"                                             "\x00"
		"mcsbw402gpo=0x88888888"                                             "\x00"
		"maxp5ga0=66,66,66,66"                                               "\x00"
		"mcsbw205glpo=0x22000000"                                            "\x00"
		"mcsbw205gmpo=0x22000000"                                            "\x00"
		"mcsbw205ghpo=0x22000000"                                            "\x00"
		"mcsbw405glpo=0x44444444"                                            "\x00"
		"mcsbw405gmpo=0x44444444"                                            "\x00"
		"mcsbw405ghpo=0x44444444"                                            "\x00"
		"mcsbw805glpo=0x66666666"                                            "\x00"
		"mcsbw805gmpo=0x66666666"                                            "\x00"
		"mcsbw805ghpo=0x66666666"                                            "\x00"

		"swctrlmap_2g=0x00000000,0x00000000,0x00000000,0x010000,0x3ff"       "\x00"
		"swctrlmap_5g=0x00100010,0x00200020,0x00200020,0x010000,0x3fe"       "\x00"
		"swctrlmapext_5g=0x00000000,0x00000000,0x00000000,0x000000,0x3"      "\x00"
		"swctrlmapext_2g=0x00000000,0x00000000,0x00000000,0x000000,0x3"      "\x00"

		"vcodivmode=1"                                                 		 "\x00"
		"deadman_to=481500000"                                               "\x00"

		"ed_thresh2g=-54"                                                 	 "\x00"
		"ed_thresh5g=-54"                                                 	 "\x00"
		"eu_edthresh2g=-54"                                                  "\x00"
		"eu_edthresh5g=-54"                                                  "\x00"
		"ldo1=4"                                                 			 "\x00"
		"rawtempsense=0x1ff"                                                 "\x00"
		"cckPwrIdxCorr=3"                                                 	 "\x00"
		"cckTssiDelay=150"                                                 	 "\x00"
		"ofdmTssiDelay=150"                                                  "\x00"
		"txpwr2gAdcScale=1"                                                  "\x00"
		"txpwr5gAdcScale=1"                                                  "\x00"
		"lowpowerrange2g=0"                                                  "\x00"
		"lowpowerrange5g=0"                                                  "\x00"
		"dot11b_opts=0x3aa85"                                                "\x00"
		"cbfilttype=2"                                                 		 "\x00"
		"fdsslevel_ch1=6"                                                    "\x00"
		"fdsslevel_ch11=4"                                                   "\x00"

		"muxenab=0x10"                                                       "\x00"
        "\x00\x00";
#ifdef __cplusplus
} /*extern "C" */
#endif

#else /* ifndef INCLUDED_NVRAM_IMAGE_H_ */

#error Wi-Fi NVRAM image included twice

#endif /* ifndef INCLUDED_NVRAM_IMAGE_H_ */
