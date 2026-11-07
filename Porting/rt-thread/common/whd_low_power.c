/***********************************************************************************************//**
 * \file whd_low_power.c
 *
 * \brief
 * WiFi Low-Power Offload MSH Commands for RT-Thread
 *
 * Provides MSH commands for WiFi low-power features:
 *   wifi_ps   - PM0/PM1/PM2 powersave & listen interval
 *   wifi_arp  - ARP offload management
 *   wifi_tko  - TCP Keepalive Offload management
 *   wifi_pf   - Packet Filter management
 *   wifi_wowl - Wake on WLAN pattern management
 *
 * Enable by defining WHD_USING_LOW_POWER in Kconfig or rtconfig.h.
 *
 ***************************************************************************************************
 * \copyright
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
 * permitted EXCLUSIVELY with Aerolynq WiFi / Bluetooth combo modules.
 * See the NOTICE and LICENSE files at the root of this package for the
 * complete terms.
 **************************************************************************************************/

#include <rtthread.h>
#include <rtdevice.h>
/* WLAN header compatibility for different RT-Thread versions */
#if (RT_VER_NUM >= 0x50200)
#include <drivers/wlan.h>
#else
#include <wlan_dev.h>
#endif
#include <string.h>
#include <stdlib.h>

#include "whd_wlan_low_power.h"
#include "whd.h"
#include "whd_int.h"
#include "whd_wifi_api.h"
#include "whd_types.h"
#include "whd_wlioctl.h"

/* lwIP includes */
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* lwIP TCP internals for manual TKO connect (access tcp_active_pcbs) */
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
/* lwIP ARP table lookup for correct destination MAC (matching Cypress LPA) */
#include "lwip/etharp.h"
/* WHD internal headers for manual TKO connect IOVAR */
#include "whd_proto.h"
#include "whd_endian.h"
#include "whd_types_int.h"

/*******************************************************************************
 * WiFi Low-Power MSH Commands (wifi_ps / wifi_arp / wifi_tko)
 *
 * Enable by defining WHD_USING_LOW_POWER in rtconfig.h or board config.
 * Provides:
 *   wifi_ps   - PM0/PM1/PM2 powersave & listen interval
 *   wifi_arp  - ARP offload management
 *   wifi_tko  - TCP Keepalive Offload management
 ******************************************************************************/

/**
 * wifi_ps - WiFi powersave and listen interval management command
 *
 * Usage:
 *   wifi_ps                         - show current powersave mode and listen interval
 *   wifi_ps mode <0|1|2>            - set powersave mode (0=off, 1=PM1, 2=PM2)
 *   wifi_ps pm2_sleep <10~2000>     - set PM2 return-to-sleep delay (ms, multiple of 10)
 *   wifi_ps listen <interval> [dtim|beacon] - set listen interval (default unit: dtim)
 */
static int cmd_wifi_ps(int argc, char **argv)
{
    whd_interface_t whd_itf = whd_lp_get_sta_itf();
    struct rt_wlan_device *wlan = whd_lp_get_sta_wlan();

    if (whd_itf == NULL)
    {
        rt_kprintf("WiFi STA not initialized\n");
        return -1;
    }

    if (argc < 2)
    {
        /* Show current status */
        uint32_t pm_mode = 0;
        whd_wifi_get_powersave_mode(whd_itf, &pm_mode);
        rt_kprintf("Powersave mode: %lu (%s)\n", pm_mode,
                   pm_mode == NO_POWERSAVE_MODE  ? "OFF" :
                   pm_mode == PM1_POWERSAVE_MODE ? "PM1 - low power" :
                   pm_mode == PM2_POWERSAVE_MODE ? "PM2 - high throughput" : "unknown");

        whd_listen_interval_t li = {0};
        if (whd_wifi_get_listen_interval(whd_itf, &li) == WHD_SUCCESS)
        {
            rt_kprintf("Listen interval: beacon=%u, dtim=%u, assoc=%u\n",
                       li.beacon, li.dtim, li.assoc);
        }
        return 0;
    }

    if (rt_strcmp(argv[1], "mode") == 0)
    {
        if (argc < 3)
        {
            uint32_t pm_mode = 0;
            whd_wifi_get_powersave_mode(whd_itf, &pm_mode);
            rt_kprintf("Powersave mode: %lu\n", pm_mode);
            return 0;
        }
        int level = atoi(argv[2]);
        if (level < 0 || level > 2)
        {
            rt_kprintf("Error: mode must be 0(off), 1(PM1), or 2(PM2)\n");
            return -1;
        }
        rt_err_t ret = whd_lp_set_powersave(wlan, level);
        if (ret == RT_EOK)
            rt_kprintf("Powersave mode set to %d\n", level);
        else
            rt_kprintf("Failed to set powersave mode\n");
    }
    else if (rt_strcmp(argv[1], "pm2_sleep") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("Usage: wifi_ps pm2_sleep <10~2000>\n");
            return -1;
        }
        uint16_t delay_ms = (uint16_t)atoi(argv[2]);
        if (delay_ms < 10 || delay_ms > 2000 || (delay_ms % 10) != 0)
        {
            rt_kprintf("Error: delay must be 10~2000, multiple of 10\n");
            return -1;
        }
        whd_result_t result = whd_wifi_enable_powersave_with_throughput(whd_itf, delay_ms);
        if (result == WHD_SUCCESS)
            rt_kprintf("PM2 enabled with return-to-sleep delay %u ms\n", delay_ms);
        else
            rt_kprintf("Failed to set PM2 sleep return time\n");
    }
    else if (rt_strcmp(argv[1], "listen") == 0)
    {
        if (argc < 3)
        {
            whd_listen_interval_t li = {0};
            if (whd_wifi_get_listen_interval(whd_itf, &li) == WHD_SUCCESS)
                rt_kprintf("Listen interval: beacon=%u, dtim=%u, assoc=%u\n",
                           li.beacon, li.dtim, li.assoc);
            else
                rt_kprintf("Failed to get listen interval\n");
            return 0;
        }
        uint8_t interval = (uint8_t)atoi(argv[2]);
        whd_listen_interval_time_unit_t unit = WHD_LISTEN_INTERVAL_TIME_UNIT_DTIM;
        if (argc >= 4 && rt_strcmp(argv[3], "beacon") == 0)
            unit = WHD_LISTEN_INTERVAL_TIME_UNIT_BEACON;

        whd_result_t result = whd_wifi_set_listen_interval(whd_itf, interval, unit);
        if (result == WHD_SUCCESS)
            rt_kprintf("Listen interval set to %u %s periods\n", interval,
                       unit == WHD_LISTEN_INTERVAL_TIME_UNIT_DTIM ? "DTIM" : "beacon");
        else
            rt_kprintf("Failed to set listen interval\n");
    }
    else
    {
        rt_kprintf("Usage: wifi_ps [command] [args]\n");
        rt_kprintf("  wifi_ps                              - show status\n");
        rt_kprintf("  wifi_ps mode <0|1|2>                 - 0=off, 1=PM1, 2=PM2\n");
        rt_kprintf("  wifi_ps pm2_sleep <10~2000>           - PM2 return-to-sleep delay (ms)\n");
        rt_kprintf("  wifi_ps listen <interval> [dtim|beacon] - set listen interval\n");
        return -1;
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_ps, wifi_ps, WiFi powersave and listen interval management);

/*******************************************************************************
 * ARP Offload MSH Command
 ******************************************************************************/

/**
 * wifi_arp - ARP offload management command
 *
 * Usage:
 *   wifi_arp                    - show ARP offload status and stats
 *   wifi_arp enable              - enable ARP offload with recommended settings
 *   wifi_arp disable             - disable ARP offload
 *   wifi_arp addip <ip>          - add host IP to offload list (e.g. 192.168.3.100)
 *   wifi_arp clearip             - clear all host IPs
 *   wifi_arp stats               - show ARP offload statistics
 *   wifi_arp stats_clear         - clear ARP offload statistics
 */
static int cmd_wifi_arp(int argc, char **argv)
{
    whd_interface_t whd_itf = whd_lp_get_sta_itf();

    if (whd_itf == NULL)
    {
        rt_kprintf("WiFi STA not initialized\n");
        return -1;
    }

    if (argc < 2)
    {
        /* Show current ARP offload status */
        uint32_t agent_en = 0, features = 0, peerage = 0, version = 0;
        whd_arp_version(whd_itf, &version);
        whd_arp_arpoe_get(whd_itf, &agent_en);
        whd_arp_features_get(whd_itf, &features);
        whd_arp_peerage_get(whd_itf, &peerage);

        rt_kprintf("ARP Offload Status:\n");
        rt_kprintf("  Version    : %lu\n", version);
        rt_kprintf("  Agent      : %s\n", agent_en ? "enabled" : "disabled");
        rt_kprintf("  Peer age   : %lu seconds\n", peerage);
        rt_kprintf("  Features   : 0x%08lx\n", features);
        rt_kprintf("    ARP_OL_AGENT          : %s\n", (features & ARP_OL_AGENT) ? "on" : "off");
        rt_kprintf("    ARP_OL_SNOOP          : %s\n", (features & ARP_OL_SNOOP) ? "on" : "off");
        rt_kprintf("    ARP_OL_HOST_AUTO_REPLY: %s\n", (features & ARP_OL_HOST_AUTO_REPLY) ? "on" : "off");
        rt_kprintf("    ARP_OL_PEER_AUTO_REPLY: %s\n", (features & ARP_OL_PEER_AUTO_REPLY) ? "on" : "off");

        /* Show host IP list */
        uint32_t ip_list[ARP_MULTIHOMING_MAX] = {0};
        uint32_t filled = 0;
        if (whd_arp_hostip_list_get(whd_itf, ARP_MULTIHOMING_MAX, ip_list, &filled) == WHD_SUCCESS)
        {
            rt_kprintf("  Host IPs   : %lu\n", filled);
            for (uint32_t i = 0; i < filled; i++)
            {
                uint8_t *ip = (uint8_t *)&ip_list[i];
                rt_kprintf("    [%lu] %d.%d.%d.%d\n", i, ip[0], ip[1], ip[2], ip[3]);
            }
        }
        return 0;
    }

    if (rt_strcmp(argv[1], "enable") == 0)
    {
        /* Enable ARP offload with recommended features:
         * - Agent enabled
         * - Snoop: learn IP-MAC from traffic
         * - Peer auto reply: chip replies to ARP from peers
         */
        uint32_t features = ARP_OL_AGENT | ARP_OL_SNOOP | ARP_OL_PEER_AUTO_REPLY;
        whd_result_t r1 = whd_arp_arpoe_set(whd_itf, 1);
        whd_result_t r2 = whd_arp_features_set(whd_itf, features);
        whd_result_t r3 = whd_arp_peerage_set(whd_itf, 1200); /* 20 min peer age */
        if (r1 == WHD_SUCCESS && r2 == WHD_SUCCESS && r3 == WHD_SUCCESS)
            rt_kprintf("ARP offload enabled (agent+snoop+peer_auto_reply, peerage=1200s)\n");
        else
            rt_kprintf("ARP offload enable failed (r1=%lu r2=%lu r3=%lu)\n", r1, r2, r3);
    }
    else if (rt_strcmp(argv[1], "disable") == 0)
    {
        if (whd_arp_arpoe_set(whd_itf, 0) == WHD_SUCCESS)
            rt_kprintf("ARP offload disabled\n");
        else
            rt_kprintf("Failed to disable ARP offload\n");
    }
    else if (rt_strcmp(argv[1], "addip") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("Usage: wifi_arp addip <ip_address>\n");
            return -1;
        }
        if (whd_arp_hostip_list_add_string(whd_itf, argv[2]) == WHD_SUCCESS)
            rt_kprintf("Host IP %s added to ARP offload\n", argv[2]);
        else
            rt_kprintf("Failed to add host IP\n");
    }
    else if (rt_strcmp(argv[1], "clearip") == 0)
    {
        if (whd_arp_hostip_list_clear(whd_itf) == WHD_SUCCESS)
            rt_kprintf("Host IP list cleared\n");
        else
            rt_kprintf("Failed to clear host IP list\n");
    }
    else if (rt_strcmp(argv[1], "stats") == 0)
    {
        whd_arp_stats_t stats;
        rt_memset(&stats, 0, sizeof(stats));
        if (whd_arp_stats_get(whd_itf, &stats) == WHD_SUCCESS)
        {
            rt_kprintf("ARP Offload Stats:\n");
            rt_kprintf("  Version       : %lu\n", stats.version);
            rt_kprintf("  Peer age      : %lu s\n", stats.peerage);
            rt_kprintf("  Agent enabled : %lu\n", stats.arpoe);
            rt_kprintf("  Features      : 0x%08lx\n", stats.features_enabled);
            rt_kprintf("  Host IP entries/overflow : %lu / %lu\n",
                       stats.stats.host_ip_entries, stats.stats.host_ip_overflow);
            rt_kprintf("  ARP table entries/overflow: %lu / %lu\n",
                       stats.stats.arp_table_entries, stats.stats.arp_table_overflow);
            rt_kprintf("  Host request/reply/service: %lu / %lu / %lu\n",
                       stats.stats.host_request, stats.stats.host_reply, stats.stats.host_service);
            rt_kprintf("  Peer request/reply/service: %lu / %lu / %lu\n",
                       stats.stats.peer_request, stats.stats.peer_reply, stats.stats.peer_service);
            rt_kprintf("  Peer request_drop/reply_drop: %lu / %lu\n",
                       stats.stats.peer_request_drop, stats.stats.peer_reply_drop);
        }
        else
            rt_kprintf("Failed to get ARP stats\n");
    }
    else if (rt_strcmp(argv[1], "stats_clear") == 0)
    {
        if (whd_arp_stats_clear(whd_itf) == WHD_SUCCESS)
            rt_kprintf("ARP stats cleared\n");
        else
            rt_kprintf("Failed to clear ARP stats\n");
    }
    else
    {
        rt_kprintf("Usage: wifi_arp [command]\n");
        rt_kprintf("  wifi_arp                - show status\n");
        rt_kprintf("  wifi_arp enable          - enable ARP offload\n");
        rt_kprintf("  wifi_arp disable         - disable ARP offload\n");
        rt_kprintf("  wifi_arp addip <ip>      - add host IP\n");
        rt_kprintf("  wifi_arp clearip         - clear all host IPs\n");
        rt_kprintf("  wifi_arp stats           - show statistics\n");
        rt_kprintf("  wifi_arp stats_clear     - clear statistics\n");
        return -1;
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_arp, wifi_arp, ARP offload management);

/*******************************************************************************
 * TCP Keepalive Offload (TKO) MSH Command
 ******************************************************************************/

static const char *tko_status_str(uint8_t status)
{
    switch (status)
    {
    case TKO_STATUS_NORMAL:                 return "NORMAL";
    case TKO_STATUS_NO_RESPONSE:            return "NO_RESPONSE";
    case TKO_STATUS_NO_TCP_ACK_FLAG:        return "NO_TCP_ACK";
    case TKO_STATUS_UNEXPECT_TCP_FLAG:      return "UNEXPECT_FLAG";
    case TKO_STATUS_SEQ_NUM_INVALID:        return "SEQ_INVALID";
    case TKO_STATUS_REMOTE_SEQ_NUM_INVALID: return "REMOTE_SEQ_INVALID";
    case TKO_STATUS_TCP_DATA:               return "TCP_DATA";
    case TKO_STATUS_UNAVAILABLE:            return "UNAVAILABLE";
    default:                                return "UNKNOWN";
    }
}

/*******************************************************************************
 * Manual TKO Connect Helpers
 *
 * Reference: Kaadas/Infineon LPA cy_whd_tko_api.c (prep_packet, tko_connect_init)
 * Keepalive packets = Ethernet(14) + IP(20) + TCP(20) = 54 bytes.
 * Both request and response are local->remote direction.
 * Request: seq = snd_nxt - 1 (probe), ack = rcv_nxt
 * Response: seq = snd_nxt (normal),   ack = rcv_nxt
 ******************************************************************************/

/* TKO event handler state */
static const uint32_t tko_events[] = { WLC_E_TKO, WLC_E_NONE };
static uint16_t tko_event_handler_index = 0xFF;
static volatile rt_bool_t tko_enabled = RT_FALSE;

/**
 * TKO event callback -- fired by FW when keepalive offload stops.
 * Possible reasons: real TCP data arrived, no response from peer, seq mismatch, etc.
 * The host should check status, process any data, then re-enable TKO if needed.
 */
static void *tko_event_handler(whd_interface_t ifp, const whd_event_header_t *event_header,
                               const uint8_t *event_data, void *handler_user_data)
{
    if (event_header != NULL && event_header->event_type == WLC_E_TKO)
    {
        tko_enabled = RT_FALSE;
        rt_kprintf("\n[TKO] Event: keepalive offload stopped (status=%lu, reason=%lu)\n",
                   (unsigned long)event_header->status, (unsigned long)event_header->reason);
        rt_kprintf("[TKO] Run 'wifi_tko status' to check, then 'wifi_tko enable' to re-arm.\n");
        rt_kprintf("msh />");
    }
    return handler_user_data;
}

#define TKO_PKT_LEN  54  /* Eth(14) + IP(20) + TCP(20) */

/* Compute IP header checksum (RFC 1071) */
static uint16_t _ip_cksum(const void *buf, int len)
{
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/**
 * Build a 54-byte raw Ethernet+IP+TCP keepalive packet (no TCP payload).
 * Matches Cypress LPA prep_packet() format exactly.
 */
static int _build_keepalive_pkt(uint8_t *pkt,
    const uint8_t *src_mac, const uint8_t *dst_mac,  /* 6 bytes each */
    uint32_t src_ip, uint32_t dst_ip,                /* network byte order */
    uint16_t src_port, uint16_t dst_port,            /* host byte order */
    uint32_t seq, uint32_t ack,                      /* host byte order */
    uint16_t window)                                 /* host byte order */
{
    rt_memset(pkt, 0, TKO_PKT_LEN);

    /* --- Ethernet Header (14 bytes) --- */
    rt_memcpy(&pkt[0], dst_mac, ETHER_ADDR_LEN);   /* destination MAC */
    rt_memcpy(&pkt[6], src_mac, ETHER_ADDR_LEN);   /* source MAC */
    pkt[12] = 0x08; pkt[13] = 0x00;                /* EtherType = IPv4 (0x0800) */

    /* --- IP Header (20 bytes at offset 14) --- */
    uint8_t *ip = &pkt[14];
    ip[0]  = 0x45;                      /* Version=4, IHL=5 */
    ip[2]  = 0x00; ip[3] = 40;          /* Total Length = 40 (IP+TCP only) */
    ip[6]  = 0x00; ip[7] = 0x00;        /* No flags, no fragment */
    ip[8]  = 32;                        /* TTL (match Cypress LPA) */
    ip[9]  = 6;                         /* Protocol = TCP */
    rt_memcpy(&ip[12], &src_ip, 4);
    rt_memcpy(&ip[16], &dst_ip, 4);
    uint16_t ip_ck = _ip_cksum(ip, 20);
    ip[10] = (uint8_t)(ip_ck & 0xFF);
    ip[11] = (uint8_t)(ip_ck >> 8);

    /* --- TCP Header (20 bytes at offset 34) --- */
    uint8_t *tcp = &pkt[34];
    tcp[0] = (uint8_t)(src_port >> 8); tcp[1] = (uint8_t)(src_port & 0xFF);
    tcp[2] = (uint8_t)(dst_port >> 8); tcp[3] = (uint8_t)(dst_port & 0xFF);
    tcp[4] = (uint8_t)(seq >> 24); tcp[5] = (uint8_t)(seq >> 16);
    tcp[6] = (uint8_t)(seq >> 8);  tcp[7] = (uint8_t)(seq & 0xFF);
    tcp[8]  = (uint8_t)(ack >> 24); tcp[9]  = (uint8_t)(ack >> 16);
    tcp[10] = (uint8_t)(ack >> 8);  tcp[11] = (uint8_t)(ack & 0xFF);
    tcp[12] = 0x50;  /* Data offset = 5 (20 bytes) */
    tcp[13] = 0x10;  /* Flags: ACK */
    tcp[14] = (uint8_t)(window >> 8); tcp[15] = (uint8_t)(window & 0xFF);

    /* TCP checksum with pseudo-header */
    uint32_t tcp_sum = 0;
    {
        const uint16_t *ip_s = (const uint16_t *)&src_ip;
        const uint16_t *ip_d = (const uint16_t *)&dst_ip;
        tcp_sum += ip_s[0]; tcp_sum += ip_s[1];
        tcp_sum += ip_d[0]; tcp_sum += ip_d[1];
        tcp_sum += htons(6);   /* protocol TCP */
        tcp_sum += htons(20);  /* TCP segment length */
    }
    {
        const uint16_t *tp = (const uint16_t *)tcp;
        for (int i = 0; i < 10; i++) tcp_sum += tp[i];
    }
    while (tcp_sum >> 16) tcp_sum = (tcp_sum & 0xFFFF) + (tcp_sum >> 16);
    uint16_t tcp_ck = (uint16_t)(~tcp_sum);
    tcp[16] = (uint8_t)(tcp_ck & 0xFF);
    tcp[17] = (uint8_t)(tcp_ck >> 8);

    return TKO_PKT_LEN;
}

/* Saved TCP connection info for TKO manual connect (extracted from tcp_pcb) */
typedef struct {
    uint32_t local_ip;    /* network byte order */
    uint32_t remote_ip;   /* network byte order */
    uint16_t local_port;  /* host byte order */
    uint16_t remote_port; /* host byte order */
    uint32_t local_seq;   /* host byte order */
    uint32_t remote_seq;  /* host byte order */
    uint16_t rx_window;   /* host byte order */
} tko_conn_info_t;

/**
 * Manually configure a TKO connection via WL_TKO_SUBCMD_CONNECT.
 * Builds Eth+IP+TCP keepalive request/response packets per Cypress LPA format.
 *
 * Key: both request and response are LOCAL->REMOTE direction.
 *   Request:  seq = local_seq - 1 (probe), ack = remote_seq
 *   Response: seq = local_seq (normal),     ack = remote_seq
 *
 * @param ifp   WHD interface
 * @param ci    Connection info (extracted from tcp_pcb)
 * @param index TKO connection index (0 to MAX_TKO_CONN-1)
 * @return WHD_SUCCESS on success
 */
static whd_result_t tko_manual_connect(whd_interface_t ifp, const tko_conn_info_t *ci, uint8_t index)
{
    whd_driver_t whd_driver;

    if (ifp == NULL || ci == NULL) return WHD_BADARG;
    whd_driver = ifp->whd_driver;
    if (whd_driver == NULL) return WHD_BADARG;

    /* Get local MAC address */
    whd_mac_t src_mac;
    if (whd_wifi_get_mac_address(ifp, &src_mac) != WHD_SUCCESS)
    {
        rt_kprintf("TKO: get MAC address failed\n");
        return WHD_BADARG;
    }

    /* Get remote MAC from ARP table (matching Cypress LPA etharp_find_addr approach).
     * For same-subnet destinations, this returns the actual server MAC.
     * For cross-subnet destinations, ARP lookup on server IP fails, so we
     * fall back to looking up the default gateway's MAC in the ARP table
     * (matching cy_wcm_get_gateway_mac_address approach). */
    whd_mac_t dst_mac;
    const char *mac_source = NULL;
    {
        struct eth_addr *eth_ret = NULL;
        const ip4_addr_t *ip_ret = NULL;
        ip4_addr_t remote_addr;
        remote_addr.addr = ci->remote_ip;  /* already in network byte order */

        LOCK_TCPIP_CORE();
        /* 1st: try server IP directly (works for same-subnet) */
        if (etharp_find_addr(NULL, &remote_addr, &eth_ret, &ip_ret) >= 0 && eth_ret != NULL)
        {
            rt_memcpy(dst_mac.octet, eth_ret->addr, ETHER_ADDR_LEN);
            mac_source = "ARP-server";
        }
        else
        {
            /* 2nd: try default gateway IP (correct next-hop for cross-subnet) */
            struct netif *nif = netif_default;
            if (nif != NULL)
            {
                ip4_addr_t gw_addr = *netif_ip4_gw(nif);
                eth_ret = NULL;
                ip_ret = NULL;
                if (!ip4_addr_isany_val(gw_addr) &&
                    etharp_find_addr(nif, &gw_addr, &eth_ret, &ip_ret) >= 0 && eth_ret != NULL)
                {
                    rt_memcpy(dst_mac.octet, eth_ret->addr, ETHER_ADDR_LEN);
                    mac_source = "ARP-gateway";
                }
            }
        }
        UNLOCK_TCPIP_CORE();
    }

    if (mac_source == NULL)
    {
        rt_kprintf("TKO: failed to resolve dst MAC (server and gateway not in ARP table)\n");
        return WHD_BADARG;
    }

    rt_kprintf("TKO: src_mac=%02X:%02X:%02X:%02X:%02X:%02X dst_mac=%02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
        src_mac.octet[0], src_mac.octet[1], src_mac.octet[2],
        src_mac.octet[3], src_mac.octet[4], src_mac.octet[5],
        dst_mac.octet[0], dst_mac.octet[1], dst_mac.octet[2],
        dst_mac.octet[3], dst_mac.octet[4], dst_mac.octet[5],
        mac_source);

    rt_kprintf("TKO connect[%u]: %d.%d.%d.%d:%u -> %d.%d.%d.%d:%u seq=0x%08lx ack=0x%08lx win=%u\n",
        index,
        ci->local_ip & 0xFF, (ci->local_ip >> 8) & 0xFF,
        (ci->local_ip >> 16) & 0xFF, (ci->local_ip >> 24) & 0xFF, ci->local_port,
        ci->remote_ip & 0xFF, (ci->remote_ip >> 8) & 0xFF,
        (ci->remote_ip >> 16) & 0xFF, (ci->remote_ip >> 24) & 0xFF, ci->remote_port,
        (unsigned long)ci->local_seq, (unsigned long)ci->remote_seq, ci->rx_window);

    uint8_t req_pkt[TKO_PKT_LEN], rsp_pkt[TKO_PKT_LEN];

    /* Request: seq = local_seq - 1 (probe), ack = remote_seq, local->remote */
    uint16_t req_len = (uint16_t)_build_keepalive_pkt(req_pkt,
        src_mac.octet, dst_mac.octet,
        ci->local_ip, ci->remote_ip, ci->local_port, ci->remote_port,
        ci->local_seq - 1, ci->remote_seq, ci->rx_window);

    /* Response: seq = local_seq (normal), ack = remote_seq, SAME local->remote direction */
    uint16_t rsp_len = (uint16_t)_build_keepalive_pkt(rsp_pkt,
        src_mac.octet, dst_mac.octet,
        ci->local_ip, ci->remote_ip, ci->local_port, ci->remote_port,
        ci->local_seq, ci->remote_seq, ci->rx_window);

    /* Construct WL_TKO_SUBCMD_CONNECT IOVAR */
    uint16_t data_payload_len = (2 * IPV4_ADDR_LEN) + req_len + rsp_len;
    uint16_t connect_len = (uint16_t)(offsetof(wl_tko_connect_t, data) + data_payload_len);

    uint32_t buf_len = (uint32_t)(WHD_PAYLOAD_MTU - strlen(IOVAR_STR_TKO) - 1);
    whd_buffer_t buffer;
    uint8_t *data = (uint8_t *)whd_proto_get_iovar_buffer(whd_driver, &buffer,
                                                          (uint16_t)buf_len, IOVAR_STR_TKO);
    if (data == NULL)
        return WHD_BUFFER_ALLOC_FAIL;

    wl_tko_t *tko = (wl_tko_t *)data;
    tko->subcmd_id = htod16(WL_TKO_SUBCMD_CONNECT);
    tko->len = htod16(TKO_DATA_OFFSET + connect_len);

    wl_tko_connect_t *conn = (wl_tko_connect_t *)tko->data;
    conn->index        = index;
    conn->ip_addr_type = 0;  /* IPv4 */
    conn->local_port   = htod16(ci->local_port);
    conn->remote_port  = htod16(ci->remote_port);
    conn->local_seq    = htod32(ci->local_seq);
    conn->remote_seq   = htod32(ci->remote_seq);
    conn->request_len  = htod16(req_len);
    conn->response_len = htod16(rsp_len);

    uint8_t *p = conn->data;
    rt_memcpy(p, &ci->local_ip, 4);   p += 4;
    rt_memcpy(p, &ci->remote_ip, 4);  p += 4;
    rt_memcpy(p, req_pkt, req_len);   p += req_len;
    rt_memcpy(p, rsp_pkt, rsp_len);

    whd_result_t result = whd_proto_set_iovar(ifp, buffer, NULL);
    if (result != WHD_SUCCESS)
        rt_kprintf("TKO: connect[%u] IOVAR failed (%lu)\n", index, (unsigned long)result);
    else
        rt_kprintf("TKO: connect[%u] configured OK\n", index);

    return result;
}

/**
 * wifi_tko - TCP Keepalive Offload management command
 *
 * Usage:
 *   wifi_tko                              - show TKO status
 *   wifi_tko enable                        - enable TKO (auto mode)
 *   wifi_tko disable                       - disable TKO
 *   wifi_tko param <interval> <retry> <retry_interval>
 *                                          - set TKO parameters (seconds)
 *   wifi_tko status                        - show keepalive status per connection
 *   wifi_tko connect <index>               - show FW connection details for index
 */
static int cmd_wifi_tko(int argc, char **argv)
{
    whd_interface_t whd_itf = whd_lp_get_sta_itf();

    if (whd_itf == NULL)
    {
        rt_kprintf("WiFi STA not initialized\n");
        return -1;
    }

    if (argc < 2)
    {
        /* Show current TKO parameters */
        whd_tko_retry_t retry = {0};
        if (whd_tko_param(whd_itf, &retry, 0) == WHD_SUCCESS)
        {
            rt_kprintf("TKO Parameters:\n");
            rt_kprintf("  Interval       : %u seconds\n", retry.tko_interval);
            rt_kprintf("  Retry count    : %u\n", retry.tko_retry_count);
            rt_kprintf("  Retry interval : %u seconds\n", retry.tko_retry_interval);
        }
        else
            rt_kprintf("Failed to get TKO parameters\n");

        uint8_t max_conn = 0;
        if (whd_tko_max_assoc(whd_itf, &max_conn) == WHD_SUCCESS)
            rt_kprintf("  Max connections: %u\n", max_conn);

        /* Show status */
        whd_tko_status_t tko_status;
        rt_memset(&tko_status, 0, sizeof(tko_status));
        if (whd_tko_get_status(whd_itf, &tko_status) == WHD_SUCCESS)
        {
            rt_kprintf("  Connection status (%u slots):\n", tko_status.count);
            for (uint8_t i = 0; i < tko_status.count && i < MAX_TKO_CONN; i++)
            {
                rt_kprintf("    [%u] %s\n", i, tko_status_str(tko_status.status[i]));
            }
        }
        return 0;
    }

    if (rt_strcmp(argv[1], "enable") == 0)
    {
        /* Auto-detect existing TCP connections and configure TKO manually.
         * FW doesn't support autoenab (subcmd 6), so we scan tcp_active_pcbs
         * and configure each ESTABLISHED connection via WL_TKO_SUBCMD_CONNECT. */
        whd_result_t ret;

        /* Disable TKO first for clean state */
        whd_tko_toggle(whd_itf, WHD_FALSE);

        /* Extract TCP connection info from lwIP PCBs (inside core lock),
         * then configure TKO connections (outside lock to avoid blocking) */
        tko_conn_info_t conns[MAX_TKO_CONN];
        uint8_t conn_count = 0;

        LOCK_TCPIP_CORE();
        {
            struct tcp_pcb *pcb;
            for (pcb = tcp_active_pcbs; pcb != NULL && conn_count < MAX_TKO_CONN; pcb = pcb->next)
            {
                if (pcb->state == ESTABLISHED)
                {
                    /* Skip connections with pending unacked/unsent data --
                     * seq/ack would be stale once the data completes */
                    if (pcb->unacked != NULL || pcb->unsent != NULL)
                    {
                        rt_kprintf("TKO: skip PCB %u.%u.%u.%u:%u (pending data)\n",
                            ip4_addr1(ip_2_ip4(&pcb->remote_ip)),
                            ip4_addr2(ip_2_ip4(&pcb->remote_ip)),
                            ip4_addr3(ip_2_ip4(&pcb->remote_ip)),
                            ip4_addr4(ip_2_ip4(&pcb->remote_ip)),
                            pcb->remote_port);
                        continue;
                    }
                    conns[conn_count].local_ip   = ip_addr_get_ip4_u32(&pcb->local_ip);
                    conns[conn_count].remote_ip  = ip_addr_get_ip4_u32(&pcb->remote_ip);
                    conns[conn_count].local_port  = pcb->local_port;
                    conns[conn_count].remote_port = pcb->remote_port;
                    conns[conn_count].local_seq   = pcb->snd_nxt;
                    conns[conn_count].remote_seq  = pcb->rcv_nxt;
                    conns[conn_count].rx_window   = pcb->rcv_wnd;
                    conn_count++;
                }
            }
        }
        UNLOCK_TCPIP_CORE();

        /* Configure each connection via IOVAR (outside core lock) */
        uint8_t ok_count = 0;
        for (uint8_t i = 0; i < conn_count; i++)
        {
            ret = tko_manual_connect(whd_itf, &conns[i], i);
            if (ret == WHD_SUCCESS)
                ok_count++;
        }
        conn_count = ok_count;

        if (conn_count == 0)
        {
            rt_kprintf("TKO: no ESTABLISHED TCP connections found\n");
            rt_kprintf("  Use 'tcpclient' first to create a TCP connection\n");
            return -1;
        }

        /* Enable TKO */
        ret = whd_tko_toggle(whd_itf, WHD_TRUE);
        if (ret == WHD_SUCCESS)
        {
            /* Register WLC_E_TKO event handler to be notified when FW
             * detects real TCP data and stops keepalive offload */
            whd_wifi_set_event_handler(whd_itf, tko_events,
                (whd_event_handler_t)tko_event_handler,
                NULL, &tko_event_handler_index);
            tko_enabled = RT_TRUE;
            rt_kprintf("TKO enabled with %u connection(s)\n", conn_count);
        }
        else
            rt_kprintf("TKO toggle failed (%lu)\n", (unsigned long)ret);
    }
    else if (rt_strcmp(argv[1], "disable") == 0)
    {
        /* Deregister TKO event handler */
        if (tko_enabled && tko_event_handler_index != 0xFF)
        {
            whd_wifi_deregister_event_handler(whd_itf, tko_event_handler_index);
            tko_event_handler_index = 0xFF;
        }
        tko_enabled = RT_FALSE;

        whd_result_t ret = whd_tko_toggle(whd_itf, WHD_FALSE);
        if (ret == WHD_SUCCESS)
            rt_kprintf("TKO disabled\n");
        else
            rt_kprintf("TKO disable failed (%lu)\n", (unsigned long)ret);
    }
    else if (rt_strcmp(argv[1], "param") == 0)
    {
        if (argc < 5)
        {
            rt_kprintf("Usage: wifi_tko param <interval_s> <retry_count> <retry_interval_s>\n");
            rt_kprintf("  Example: wifi_tko param 60 3 10\n");
            rt_kprintf("  (send every 60s, retry 3 times, 10s between retries)\n");
            return -1;
        }
        whd_tko_retry_t retry;
        retry.tko_interval       = (uint16_t)atoi(argv[2]);
        retry.tko_retry_count    = (uint16_t)atoi(argv[3]);
        retry.tko_retry_interval = (uint16_t)atoi(argv[4]);

        if (retry.tko_interval == 0 || retry.tko_retry_count == 0)
        {
            rt_kprintf("Error: interval and retry_count must be > 0\n");
            return -1;
        }

        whd_result_t ret = whd_tko_param(whd_itf, &retry, 1);
        if (ret == WHD_SUCCESS)
            rt_kprintf("TKO params set: interval=%us, retries=%u, retry_interval=%us\n",
                       retry.tko_interval, retry.tko_retry_count, retry.tko_retry_interval);
        else
            rt_kprintf("Failed to set TKO params (%lu)\n", ret);
    }
    else if (rt_strcmp(argv[1], "status") == 0)
    {
        whd_tko_status_t tko_status;
        rt_memset(&tko_status, 0, sizeof(tko_status));
        if (whd_tko_get_status(whd_itf, &tko_status) == WHD_SUCCESS)
        {
            rt_kprintf("TKO Status (%u connections):\n", tko_status.count);
            for (uint8_t i = 0; i < tko_status.count && i < MAX_TKO_CONN; i++)
            {
                rt_kprintf("  [%u] %s (code=%u)\n", i,
                           tko_status_str(tko_status.status[i]),
                           tko_status.status[i]);
            }
        }
        else
            rt_kprintf("Failed to get TKO status\n");
    }
    else if (rt_strcmp(argv[1], "connect") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("Usage: wifi_tko connect <index>  (0~3)\n");
            return -1;
        }
        uint8_t index = (uint8_t)atoi(argv[2]);
        if (index >= MAX_TKO_CONN)
        {
            rt_kprintf("Error: index must be 0~%d\n", MAX_TKO_CONN - 1);
            return -1;
        }

        /* Allocate enough space for connect struct + IP addrs + keepalive packets */
        uint8_t buf[512];
        rt_memset(buf, 0, sizeof(buf));
        whd_tko_connect_t *conn = (whd_tko_connect_t *)buf;

        whd_result_t ret = whd_tko_get_FW_connect(whd_itf, index, conn, sizeof(buf));
        if (ret == WHD_SUCCESS)
        {
            rt_kprintf("TKO Connection [%u]:\n", index);
            rt_kprintf("  IP type     : %s\n", conn->ip_addr_type == 0 ? "IPv4" : "IPv6");
            rt_kprintf("  Local port  : %u\n", conn->local_port);
            rt_kprintf("  Remote port : %u\n", conn->remote_port);
            rt_kprintf("  Local seq   : %lu\n", (unsigned long)conn->local_seq);
            rt_kprintf("  Remote seq  : %lu\n", (unsigned long)conn->remote_seq);
            rt_kprintf("  Request len : %u\n", conn->request_len);
            rt_kprintf("  Response len: %u\n", conn->response_len);

            if (conn->ip_addr_type == 0)  /* IPv4 */
            {
                uint8_t *lip = &conn->data[0];
                uint8_t *rip = &conn->data[4];
                rt_kprintf("  Local IP    : %d.%d.%d.%d\n", lip[0], lip[1], lip[2], lip[3]);
                rt_kprintf("  Remote IP   : %d.%d.%d.%d\n", rip[0], rip[1], rip[2], rip[3]);
            }
        }
        else
            rt_kprintf("Failed to get connection info (index=%u, err=%lu)\n", index, ret);
    }
    else
    {
        rt_kprintf("Usage: wifi_tko [command]\n");
        rt_kprintf("  wifi_tko                 - show TKO parameters and status\n");
        rt_kprintf("  wifi_tko enable           - enable TCP keepalive offload\n");
        rt_kprintf("  wifi_tko disable          - disable TCP keepalive offload\n");
        rt_kprintf("  wifi_tko param <i> <r> <ri> - set interval/retry/retry_interval (s)\n");
        rt_kprintf("  wifi_tko status           - show per-connection keepalive status\n");
        rt_kprintf("  wifi_tko connect <0~3>    - show FW connection details\n");
        return -1;
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_tko, wifi_tko, TCP Keepalive Offload management);

/******************************************************************************
 * Packet Filter (PF) MSH Command
 *
 * Set packet filter rules in the WiFi FW for low-power sleep to reduce
 * unnecessary OOB wake-ups.
 *
 * ===== Broadcom/Cypress FW Filter Engine Principle =====
 *
 *   The FW has a "pkt_filter_mode" (master mode) global switch:
 *     mode=1 (FORWARD_ON_MATCH, default) = whitelist mode
 *       - Packets matching any POSITIVE (keep) filter -> forwarded to Host (trigger OOB)
 *       - Packets not matching any filter -> silently dropped by FW (no OOB)
 *       - Multiple filters use OR logic: match any one to pass
 *
 *   Only add POSITIVE (keep) rules to list "wanted packets",
 *   all other traffic is auto-dropped by FW, no catch-all rule needed.
 *
 * Reference: Infineon LPA 5.8.1 cy_lpa_wifi_pf_ol.c
 *            Infineon WLAN_Offloads CE229926 cycfg_connectivity_wifi.c
 ******************************************************************************/

/* Packet filter byte offsets / lengths (Ethernet frame layout) */
#define PF_ETHTYPE_OFFSET      12   /* EtherType starts 12 bytes into Ethernet hdr */
#define PF_ETHTYPE_LEN          2   /* EtherType field is 2 bytes */
#define PF_IPV4_HDR_LEN        20   /* Minimum IPv4 header length (no options) */
#define PF_UDP_HDR_PORT_LEN     4   /* src_port(2) + dst_port(2) in UDP/TCP header */
/* PORT_FILTER_LEN = EtherType(2) + IPv4Hdr(20) + UDP/TCP ports(4) = 26 */
#define PF_PORT_FILTER_LEN    (PF_ETHTYPE_LEN + PF_IPV4_HDR_LEN + PF_UDP_HDR_PORT_LEN)

#define PF_IP_PROTO_TCP         6
#define PF_IP_PROTO_UDP        17

/* Scratch buffers for building filter mask/pattern (max 64 bytes each) */
static uint8_t pf_mask_buf[64];
static uint8_t pf_pat_buf[64];

/**
 * Parse a hex string (e.g. "0800" or "0x0800") into a byte array.
 * Returns number of bytes parsed, or -1 on error.
 */
static int pf_parse_hex(const char *hex_str, uint8_t *out, int max_len)
{
    const char *p = hex_str;
    int len = 0;

    /* Skip optional "0x" or "0X" prefix */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;

    int slen = rt_strlen(p);
    if (slen == 0 || (slen % 2) != 0)
        return -1;  /* must be even number of hex chars */

    for (int i = 0; i < slen && len < max_len; i += 2, len++)
    {
        uint8_t hi, lo;
        char ch = p[i];
        char cl = p[i + 1];

        if      (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        else return -1;

        if      (cl >= '0' && cl <= '9') lo = cl - '0';
        else if (cl >= 'a' && cl <= 'f') lo = cl - 'a' + 10;
        else if (cl >= 'A' && cl <= 'F') lo = cl - 'A' + 10;
        else return -1;

        out[len] = (hi << 4) | lo;
    }
    return len;
}

/**
 * Build and add an EtherType packet filter.
 * Matches at offset 12 (EtherType field), 2-byte mask = FF FF.
 */
static int pf_add_ethtype(whd_interface_t ifp, uint32_t id, uint16_t ethtype,
                          whd_packet_filter_rule_t rule)
{
    whd_packet_filter_t filter;

    rt_memset(pf_mask_buf, 0, sizeof(pf_mask_buf));
    rt_memset(pf_pat_buf,  0, sizeof(pf_pat_buf));

    /* Mask: match all 16 bits of EtherType */
    pf_mask_buf[0] = 0xFF;
    pf_mask_buf[1] = 0xFF;

    /* Pattern: the EtherType value in network byte order */
    pf_pat_buf[0] = (uint8_t)(ethtype >> 8);
    pf_pat_buf[1] = (uint8_t)(ethtype & 0xFF);

    filter.id        = id;
    filter.rule      = rule;
    filter.offset    = PF_ETHTYPE_OFFSET;
    filter.mask_size = PF_ETHTYPE_LEN;
    filter.mask      = pf_mask_buf;
    filter.pattern   = pf_pat_buf;

    whd_result_t ret = whd_pf_add_packet_filter(ifp, &filter);
    if (ret != WHD_SUCCESS)
    {
        rt_kprintf("Failed to add EtherType filter (id=%lu, err=%lu)\n",
                   (unsigned long)id, (unsigned long)ret);
        return -1;
    }
    rt_kprintf("Added EtherType filter: id=%lu ethtype=0x%04X rule=%s\n",
               (unsigned long)id, ethtype,
               rule == WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING ? "KEEP" : "DISCARD");
    return 0;
}

/**
 * Build and add a port packet filter.
 * Matches: EtherType(IP=0x0800) + IP protocol(TCP/UDP) + src/dst port
 * Total filter length = 26 bytes from offset 12.
 */
static int pf_add_port(whd_interface_t ifp, uint32_t id, uint8_t proto,
                       int is_src, uint16_t port,
                       whd_packet_filter_rule_t rule)
{
    whd_packet_filter_t filter;

    rt_memset(pf_mask_buf, 0, sizeof(pf_mask_buf));
    rt_memset(pf_pat_buf,  0, sizeof(pf_pat_buf));

    /* Byte 0-1: EtherType = 0x0800 (IPv4) */
    pf_mask_buf[0] = 0xFF; pf_mask_buf[1] = 0xFF;
    pf_pat_buf[0]  = 0x08; pf_pat_buf[1]  = 0x00;

    /* Byte 2-21: IPv4 header (20 bytes), protocol at byte 9 (= index 2+9 = 11) */
    pf_mask_buf[2 + 9] = 0xFF;
    pf_pat_buf[2 + 9]  = proto;

    /* Byte 22-25: src_port(2) + dst_port(2) */
    if (is_src)
    {
        /* Source port at byte offset 22-23 */
        pf_mask_buf[22] = 0xFF; pf_mask_buf[23] = 0xFF;
        pf_pat_buf[22]  = (uint8_t)(port >> 8);
        pf_pat_buf[23]  = (uint8_t)(port & 0xFF);
    }
    else
    {
        /* Destination port at byte offset 24-25 */
        pf_mask_buf[24] = 0xFF; pf_mask_buf[25] = 0xFF;
        pf_pat_buf[24]  = (uint8_t)(port >> 8);
        pf_pat_buf[25]  = (uint8_t)(port & 0xFF);
    }

    filter.id        = id;
    filter.rule      = rule;
    filter.offset    = PF_ETHTYPE_OFFSET;
    filter.mask_size = PF_PORT_FILTER_LEN;
    filter.mask      = pf_mask_buf;
    filter.pattern   = pf_pat_buf;

    whd_result_t ret = whd_pf_add_packet_filter(ifp, &filter);
    if (ret != WHD_SUCCESS)
    {
        rt_kprintf("Failed to add port filter (id=%lu, err=%lu)\n",
                   (unsigned long)id, (unsigned long)ret);
        return -1;
    }
    rt_kprintf("Added port filter: id=%lu proto=%s %s_port=%u rule=%s\n",
               (unsigned long)id,
               proto == PF_IP_PROTO_TCP ? "TCP" : "UDP",
               is_src ? "src" : "dst",
               port,
               rule == WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING ? "KEEP" : "DISCARD");
    return 0;
}

/**
 * wifi_pf MSH command - Packet Filter management
 */
static int cmd_wifi_pf(int argc, char **argv)
{
    whd_interface_t ifp = whd_lp_get_sta_itf();

    if (ifp == NULL)
    {
        rt_kprintf("WiFi STA not initialized\n");
        return -1;
    }

    if (argc < 2)
        goto usage;

    /* ---- wifi_pf add ---- */
    if (rt_strcmp(argv[1], "add") == 0)
    {
        if (argc < 4)
        {
            rt_kprintf("Usage: wifi_pf add <id> <ethtype|port|raw> ...\n");
            return -1;
        }

        uint32_t id = (uint32_t)atoi(argv[2]);
        const char *type = argv[3];
        whd_packet_filter_rule_t rule = WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING; /* default: keep (whitelist) */

        /* --- ethtype --- */
        if (rt_strcmp(type, "ethtype") == 0)
        {
            if (argc < 5)
            {
                rt_kprintf("Usage: wifi_pf add <id> ethtype <hex> [keep|discard]\n");
                rt_kprintf("  Example: wifi_pf add 0 ethtype 0806          (keep ARP)\n");
                rt_kprintf("  Example: wifi_pf add 5 ethtype 86DD discard  (discard IPv6, single only!)\n");
                return -1;
            }
            uint16_t ethtype = (uint16_t)strtoul(argv[4], NULL, 16);
            if (argc >= 6 && rt_strcmp(argv[5], "discard") == 0)
                rule = WHD_PACKET_FILTER_RULE_NEGATIVE_MATCHING;
            else
                rule = WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING;  /* keep as default (whitelist) */
            return pf_add_ethtype(ifp, id, ethtype, rule);
        }
        /* --- port --- */
        else if (rt_strcmp(type, "port") == 0)
        {
            if (argc < 7)
            {
                rt_kprintf("Usage: wifi_pf add <id> port <tcp|udp> <src|dst> <port> [keep|discard]\n");
                rt_kprintf("  Example: wifi_pf add 1 port tcp src 3353 keep (keep TCP src 3353)\n");
                rt_kprintf("  Example: wifi_pf add 2 port tcp dst 3353 keep (keep TCP dst 3353)\n");
                return -1;
            }
            uint8_t proto;
            if (rt_strcmp(argv[4], "tcp") == 0)
                proto = PF_IP_PROTO_TCP;
            else if (rt_strcmp(argv[4], "udp") == 0)
                proto = PF_IP_PROTO_UDP;
            else
            {
                rt_kprintf("Unknown protocol '%s', use 'tcp' or 'udp'\n", argv[4]);
                return -1;
            }

            int is_src;
            if (rt_strcmp(argv[5], "src") == 0)
                is_src = 1;
            else if (rt_strcmp(argv[5], "dst") == 0)
                is_src = 0;
            else
            {
                rt_kprintf("Unknown direction '%s', use 'src' or 'dst'\n", argv[5]);
                return -1;
            }

            uint16_t port = (uint16_t)atoi(argv[6]);
            if (argc >= 8 && rt_strcmp(argv[7], "discard") == 0)
                rule = WHD_PACKET_FILTER_RULE_NEGATIVE_MATCHING;
            else
                rule = WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING;  /* keep as default (whitelist) */
            return pf_add_port(ifp, id, proto, is_src, port, rule);
        }
        /* --- raw --- */
        else if (rt_strcmp(type, "raw") == 0)
        {
            if (argc < 7)
            {
                rt_kprintf("Usage: wifi_pf add <id> raw <offset> <mask_hex> <pattern_hex> [keep|discard]\n");
                rt_kprintf("  <mask_hex>/<pattern_hex> are hex strings (e.g. FFFF or 0xFFFF)\n");
                rt_kprintf("  Example: wifi_pf add 10 raw 12 FFFF 0806   (keep ARP at ethtype)\n");
                return -1;
            }
            uint16_t offset = (uint16_t)atoi(argv[4]);

            int mask_len = pf_parse_hex(argv[5], pf_mask_buf, sizeof(pf_mask_buf));
            if (mask_len <= 0)
            {
                rt_kprintf("Invalid mask hex string '%s'\n", argv[5]);
                return -1;
            }
            int pat_len = pf_parse_hex(argv[6], pf_pat_buf, sizeof(pf_pat_buf));
            if (pat_len <= 0 || pat_len != mask_len)
            {
                rt_kprintf("Invalid pattern hex or length mismatch (mask=%d, pat=%d)\n",
                           mask_len, pat_len);
                return -1;
            }
            if (argc >= 8 && rt_strcmp(argv[7], "discard") == 0)
                rule = WHD_PACKET_FILTER_RULE_NEGATIVE_MATCHING;
            else
                rule = WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING;  /* keep as default (whitelist) */

            whd_packet_filter_t filter;
            filter.id        = id;
            filter.rule      = rule;
            filter.offset    = offset;
            filter.mask_size = (uint16_t)mask_len;
            filter.mask      = pf_mask_buf;
            filter.pattern   = pf_pat_buf;

            whd_result_t ret = whd_pf_add_packet_filter(ifp, &filter);
            if (ret != WHD_SUCCESS)
            {
                rt_kprintf("Failed to add raw filter (id=%lu, err=%lu)\n",
                           (unsigned long)id, (unsigned long)ret);
                return -1;
            }
            rt_kprintf("Added raw filter: id=%lu offset=%u len=%d rule=%s\n",
                       (unsigned long)id, offset, mask_len,
                       rule == WHD_PACKET_FILTER_RULE_POSITIVE_MATCHING ? "KEEP" : "DISCARD");
            return 0;
        }
        else
        {
            rt_kprintf("Unknown filter type '%s', use 'ethtype', 'port', or 'raw'\n", type);
            return -1;
        }
    }
    /* ---- wifi_pf remove ---- */
    else if (rt_strcmp(argv[1], "remove") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf remove <id>\n"); return -1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        whd_result_t ret = whd_pf_remove_packet_filter(ifp, id);
        rt_kprintf("Remove filter %u: %s (err=%lu)\n", id,
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    /* ---- wifi_pf enable ---- */
    else if (rt_strcmp(argv[1], "enable") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf enable <id>\n"); return -1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        whd_result_t ret = whd_pf_enable_packet_filter(ifp, id);
        rt_kprintf("Enable filter %u: %s (err=%lu)\n", id,
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    /* ---- wifi_pf disable ---- */
    else if (rt_strcmp(argv[1], "disable") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf disable <id>\n"); return -1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        whd_result_t ret = whd_pf_disable_packet_filter(ifp, id);
        rt_kprintf("Disable filter %u: %s (err=%lu)\n", id,
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    /* ---- wifi_pf stats ---- */
    else if (rt_strcmp(argv[1], "stats") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf stats <id>\n"); return -1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        whd_pkt_filter_stats_t stats;
        rt_memset(&stats, 0, sizeof(stats));
        whd_result_t ret = whd_pf_get_packet_filter_stats(ifp, id, &stats);
        if (ret != WHD_SUCCESS)
        {
            rt_kprintf("Failed to get stats for filter %u (err=%lu)\n", id, (unsigned long)ret);
            return -1;
        }
        rt_kprintf("Filter %u stats:\n", id);
        rt_kprintf("  Matched   : %lu\n", (unsigned long)stats.num_pkts_matched);
        rt_kprintf("  Forwarded : %lu\n", (unsigned long)stats.num_pkts_forwarded);
        rt_kprintf("  Discarded : %lu\n", (unsigned long)stats.num_pkts_discarded);
        return 0;
    }
    /* ---- wifi_pf get ---- */
    else if (rt_strcmp(argv[1], "get") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf get <id>\n"); return -1; }
        uint8_t id = (uint8_t)atoi(argv[2]);
        uint8_t mask[64], pattern[64];
        uint32_t size_out = 0;
        whd_result_t ret = whd_pf_get_packet_filter_mask_and_pattern(ifp, id, sizeof(mask),
                                                                      mask, pattern, &size_out);
        if (ret != WHD_SUCCESS)
        {
            rt_kprintf("Failed to get filter %u (err=%lu)\n", id, (unsigned long)ret);
            return -1;
        }
        rt_kprintf("Filter %u (len=%lu):\n", id, (unsigned long)size_out);
        rt_kprintf("  Mask   : ");
        for (uint32_t i = 0; i < size_out; i++)
            rt_kprintf("%02X", mask[i]);
        rt_kprintf("\n  Pattern: ");
        for (uint32_t i = 0; i < size_out; i++)
            rt_kprintf("%02X", pattern[i]);
        rt_kprintf("\n");
        return 0;
    }
    /* ---- wifi_pf clear ---- */
    else if (rt_strcmp(argv[1], "clear") == 0)
    {
        if (argc < 3) { rt_kprintf("Usage: wifi_pf clear <id>\n"); return -1; }
        uint32_t id = (uint32_t)atoi(argv[2]);
        whd_result_t ret = whd_wifi_clear_packet_filter_stats(ifp, id);
        rt_kprintf("Clear stats for filter %lu: %s (err=%lu)\n",
                   (unsigned long)id, ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    else
        goto usage;

    return 0;

usage:
    rt_kprintf("Usage: wifi_pf [command]\n");
    rt_kprintf("  wifi_pf add <id> ethtype <hex> [keep|discard]\n");
    rt_kprintf("      Add EtherType filter (default: keep)\n");
    rt_kprintf("  wifi_pf add <id> port <tcp|udp> <src|dst> <port> [keep|discard]\n");
    rt_kprintf("      Add TCP/UDP port filter (default: keep)\n");
    rt_kprintf("  wifi_pf add <id> raw <offset> <mask_hex> <pattern_hex> [keep|discard]\n");
    rt_kprintf("      Add raw byte-pattern filter (default: keep)\n");
    rt_kprintf("  wifi_pf remove <id>   - remove filter\n");
    rt_kprintf("  wifi_pf enable <id>   - enable filter\n");
    rt_kprintf("  wifi_pf disable <id>  - disable filter\n");
    rt_kprintf("  wifi_pf stats <id>    - show filter match statistics\n");
    rt_kprintf("  wifi_pf get <id>      - dump filter mask and pattern\n");
    rt_kprintf("  wifi_pf clear <id>    - clear filter statistics\n");
    rt_kprintf("\nFilter mode (FW default = whitelist / FORWARD_ON_MATCH):\n");
    rt_kprintf("  keep (default) = whitelist, only matching packets forwarded to host\n");
    rt_kprintf("  discard        = blacklist, WARNING: multiple discard rules cancel each other!\n");
    rt_kprintf("\nTypical usage (whitelist, match official WLAN_Offloads demo):\n");
    rt_kprintf("  wifi_pf add 0 ethtype 0806                 # keep ARP\n");
    rt_kprintf("  wifi_pf add 1 port tcp src 3353             # keep TCP src 3353\n");
    rt_kprintf("  wifi_pf add 2 port tcp dst 3353             # keep TCP dst 3353\n");
    rt_kprintf("  wifi_pf add 3 port tcp src 3360             # keep TCP src 3360\n");
    rt_kprintf("  wifi_pf add 4 port tcp dst 3360             # keep TCP dst 3360\n");
    rt_kprintf("  wifi_pf enable 0\n");
    rt_kprintf("  wifi_pf enable 1\n");
    rt_kprintf("  wifi_pf enable 2\n");
    rt_kprintf("  wifi_pf enable 3\n");
    rt_kprintf("  wifi_pf enable 4\n");
    rt_kprintf("  # All other packets (IPv6/mDNS/NetBIOS) auto-discarded by FW\n");
    rt_kprintf("  # Each port needs BOTH src AND dst direction filters!\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_pf, wifi_pf, Packet Filter management);

/******************************************************************************
 * Wake on WLAN (WOWL) MSH Command
 *
 * Configures WLAN FW to wake the host on specific packet patterns.
 ******************************************************************************/

/* Extern WHD WOWL APIs (implemented in whd_wifi_api.c) */
extern whd_result_t whd_get_wowl_cap(whd_interface_t ifp, uint32_t *value);
extern whd_result_t whd_set_wowl_cap(whd_interface_t ifp, uint32_t value);
extern whd_result_t whd_wowl_clear(whd_interface_t ifp);
extern whd_result_t whd_wowl_activate(whd_interface_t ifp, uint32_t value);
extern whd_result_t whd_set_wowl_pattern(whd_interface_t ifp, char* opt, uint32_t offset,
                    uint8_t mask_size, uint8_t *mask, uint8_t pattern_size,
                    uint8_t *pattern, uint8_t type);

/* WOWL flag names for pretty-printing */
static const struct {
    uint32_t    flag;
    const char *name;
} wowl_flag_table[] = {
    { (1 <<  0), "magic"   },   /* WL_WOWL_MAGIC */
    { (1 <<  1), "net"     },   /* WL_WOWL_NET   */
    { (1 <<  2), "dis"     },   /* WL_WOWL_DIS   */
    { (1 <<  3), "retr"    },   /* WL_WOWL_RETR  */
    { (1 <<  4), "bcn"     },   /* WL_WOWL_BCN   */
    { (1 << 12), "arp"     },   /* WL_WOWL_ARPOFFLOAD */
    { (1 << 15), "bcast"   },   /* WL_WOWL_BCAST */
};
#define WOWL_FLAG_TABLE_SIZE  (sizeof(wowl_flag_table) / sizeof(wowl_flag_table[0]))

/* Scratch buffers for WOWL pattern */
static uint8_t wowl_mask_buf[64];
static uint8_t wowl_pat_buf[64];

/**
 * Parse a WOWL flag name string to a bitmask value.
 */
static uint32_t wowl_parse_flag(const char *name)
{
    for (uint32_t i = 0; i < WOWL_FLAG_TABLE_SIZE; i++)
    {
        if (rt_strcmp(name, wowl_flag_table[i].name) == 0)
            return wowl_flag_table[i].flag;
    }
    /* Try as hex number */
    return (uint32_t)strtoul(name, NULL, 0);
}

/**
 * Print WOWL flags as human-readable flag names.
 */
static void wowl_print_flags(uint32_t flags)
{
    if (flags == 0) { rt_kprintf("(none)"); return; }
    int first = 1;
    for (uint32_t i = 0; i < WOWL_FLAG_TABLE_SIZE; i++)
    {
        if (flags & wowl_flag_table[i].flag)
        {
            rt_kprintf("%s%s", first ? "" : "|", wowl_flag_table[i].name);
            first = 0;
            flags &= ~wowl_flag_table[i].flag;
        }
    }
    if (flags) /* remaining unknown bits */
        rt_kprintf("%s0x%lx", first ? "" : "|", (unsigned long)flags);
}

/**
 * wifi_wowl MSH command - Wake on WLAN management
 */
static int cmd_wifi_wowl(int argc, char **argv)
{
    whd_interface_t ifp = whd_lp_get_sta_itf();

    if (ifp == NULL)
    {
        rt_kprintf("WiFi STA not initialized\n");
        return -1;
    }

    /* ---- wifi_wowl (no args) - show status ---- */
    if (argc < 2)
    {
        uint32_t wowl = 0;
        whd_result_t ret = whd_get_wowl_cap(ifp, &wowl);
        if (ret != WHD_SUCCESS)
        {
            rt_kprintf("Failed to get WOWL status (err=%lu)\n", (unsigned long)ret);
            return -1;
        }
        rt_kprintf("WOWL capability flags: 0x%08lX = ", (unsigned long)wowl);
        wowl_print_flags(wowl);
        rt_kprintf("\n");
        return 0;
    }

    /* ---- wifi_wowl set <flag> [flag2 ...] ---- */
    if (rt_strcmp(argv[1], "set") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("Usage: wifi_wowl set <flag> [flag2 ...]\n");
            rt_kprintf("  Flags: magic net dis retr bcn arp bcast  (or hex value)\n");
            rt_kprintf("  Example: wifi_wowl set magic net\n");
            return -1;
        }
        uint32_t flags = 0;
        for (int i = 2; i < argc; i++)
            flags |= wowl_parse_flag(argv[i]);

        whd_result_t ret = whd_set_wowl_cap(ifp, flags);
        if (ret != WHD_SUCCESS)
        {
            rt_kprintf("Failed to set WOWL flags (err=%lu)\n", (unsigned long)ret);
            return -1;
        }
        rt_kprintf("WOWL flags set: 0x%08lX = ", (unsigned long)flags);
        wowl_print_flags(flags);
        rt_kprintf("\n");
        return 0;
    }
    /* ---- wifi_wowl pattern ---- */
    else if (rt_strcmp(argv[1], "pattern") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("Usage: wifi_wowl pattern <add|del|clear> ...\n");
            return -1;
        }

        /* ---- pattern clear ---- */
        if (rt_strcmp(argv[2], "clear") == 0)
        {
            whd_result_t ret = whd_wowl_clear(ifp);
            rt_kprintf("WOWL pattern clear: %s (err=%lu)\n",
                       ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
            return (ret == WHD_SUCCESS) ? 0 : -1;
        }

        /* ---- pattern add / del ---- */
        if (argc < 6)
        {
            rt_kprintf("Usage: wifi_wowl pattern <add|del> <offset> <mask_hex> <pattern_hex>\n");
            rt_kprintf("  <mask_hex>/<pattern_hex> are hex strings without 0x prefix\n");
            rt_kprintf("  The mask bits indicate which pattern bytes to match.\n");
            rt_kprintf("  Example: wifi_wowl pattern add 0 FF08 0800     (match IP ethtype)\n");
            return -1;
        }

        char *mode;
        if (rt_strcmp(argv[2], "add") == 0)
            mode = "add";
        else if (rt_strcmp(argv[2], "del") == 0)
            mode = "del";
        else
        {
            rt_kprintf("Unknown pattern op '%s', use 'add', 'del', or 'clear'\n", argv[2]);
            return -1;
        }

        uint32_t offset = (uint32_t)atoi(argv[3]);

        int mask_len = pf_parse_hex(argv[4], wowl_mask_buf, sizeof(wowl_mask_buf));
        if (mask_len <= 0)
        {
            rt_kprintf("Invalid mask hex '%s'\n", argv[4]);
            return -1;
        }

        int pat_len = pf_parse_hex(argv[5], wowl_pat_buf, sizeof(wowl_pat_buf));
        if (pat_len <= 0)
        {
            rt_kprintf("Invalid pattern hex '%s'\n", argv[5]);
            return -1;
        }

        /* Reverse bits in each mask byte to match FW format (LSB first) */
        for (int i = 0; i < mask_len; i++)
        {
            uint8_t b = wowl_mask_buf[i];
            uint8_t r = 0;
            for (int k = 0; k < 8; k++)
                r |= ((b >> k) & 1) << (7 - k);
            wowl_mask_buf[i] = r;
        }

        whd_result_t ret = whd_set_wowl_pattern(ifp, mode, offset,
                                                  (uint8_t)mask_len, wowl_mask_buf,
                                                  (uint8_t)pat_len, wowl_pat_buf,
                                                  0 /* WOWL_PATTERN_TYPE_BITMAP */);
        if (ret != WHD_SUCCESS)
        {
            rt_kprintf("WOWL pattern %s failed (err=%lu)\n", mode, (unsigned long)ret);
            return -1;
        }
        rt_kprintf("WOWL pattern %s: offset=%lu mask_len=%d pat_len=%d OK\n",
                   mode, (unsigned long)offset, mask_len, pat_len);
        return 0;
    }
    /* ---- wifi_wowl activate ---- */
    else if (rt_strcmp(argv[1], "activate") == 0)
    {
        whd_result_t ret = whd_wowl_activate(ifp, 1);
        rt_kprintf("WOWL activate: %s (err=%lu)\n",
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    /* ---- wifi_wowl deactivate ---- */
    else if (rt_strcmp(argv[1], "deactivate") == 0)
    {
        whd_result_t ret = whd_wowl_activate(ifp, 0);
        rt_kprintf("WOWL deactivate: %s (err=%lu)\n",
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    /* ---- wifi_wowl clear ---- */
    else if (rt_strcmp(argv[1], "clear") == 0)
    {
        whd_result_t ret = whd_wowl_clear(ifp);
        rt_kprintf("WOWL clear: %s (err=%lu)\n",
                   ret == WHD_SUCCESS ? "OK" : "FAILED", (unsigned long)ret);
        return (ret == WHD_SUCCESS) ? 0 : -1;
    }
    else
    {
        rt_kprintf("Usage: wifi_wowl [command]\n");
        rt_kprintf("  wifi_wowl                   - show WOWL status/flags\n");
        rt_kprintf("  wifi_wowl set <flags...>    - set capability flags\n");
        rt_kprintf("      Flags: magic net dis retr bcn arp bcast (or hex)\n");
        rt_kprintf("  wifi_wowl pattern add <offset> <mask_hex> <pattern_hex>\n");
        rt_kprintf("      Add a wake-up pattern\n");
        rt_kprintf("  wifi_wowl pattern del <offset> <mask_hex> <pattern_hex>\n");
        rt_kprintf("      Remove a wake-up pattern\n");
        rt_kprintf("  wifi_wowl pattern clear     - clear all patterns\n");
        rt_kprintf("  wifi_wowl activate          - arm WOWL (before sleep)\n");
        rt_kprintf("  wifi_wowl deactivate        - disarm WOWL (after wake)\n");
        rt_kprintf("  wifi_wowl clear             - reset WOWL settings\n");
        rt_kprintf("\nTypical flow:\n");
        rt_kprintf("  wifi_wowl set magic net       # enable magic pkt + net pattern\n");
        rt_kprintf("  wifi_wowl pattern add 0 FF 08  # add pattern\n");
        rt_kprintf("  wifi_wowl activate             # arm before sleep\n");
        rt_kprintf("  ... (MCU sleeps, FW monitors)  \n");
        rt_kprintf("  wifi_wowl deactivate           # after wake\n");
        rt_kprintf("  wifi_wowl pattern clear        # clean up\n");
        return -1;
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_wowl, wifi_wowl, Wake on WLAN pattern management);
