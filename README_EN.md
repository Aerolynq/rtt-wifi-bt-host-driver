# WiFi-BT Host Driver for RT-Thread

WiFi 4/5/6 and Bluetooth host driver for RT-Thread, built on Infineon's WHD and BTStack, for Aerolynq CYWL series combo modules over SDIO + HCI UART.
**License**: Proprietary (Aerolynq, hardware-restricted) + Apache-2.0 (upstream components).

Host driver software package for Aerolynq® **CYWL series WiFi + Bluetooth combo modules** on RT-Thread:

- **WiFi**: based on Infineon WHD, connected over SDIO, registered as an RT-Thread WLAN device.
  It can directly use the `rt_wlan_*` API with LwIP for networking, and supports WiFi 4 / 5 / 6 modules.
- **Bluetooth**: based on Infineon BTStack, connected over HCI UART, supporting BLE and optionally classic Bluetooth dual mode.

**License**: Proprietary (Aerolynq® proprietary, restricted to use with Aerolynq® modules) + Apache-2.0 (upstream components).
See [`LICENSE`](LICENSE) / [`NOTICE`](NOTICE) for the complete terms and per-file attribution.

---

## 1. Introduction

The supported Aerolynq® **CYWL series modules** are as follows:

| Module    | WiFi                   | Band           | Bandwidth     | WPA3 | Bluetooth | Host Interface |
| --------- | ---------------------- | -------------- | ------------- | ---- | --------- | -------------- |
| CYWL6208  | WiFi 4 (b/g/n)         | 2.4G only      | 20MHz         | No   | BT/BLE    | SDIO + UART    |
| CYWL6209  | WiFi 4 (b/g/n)         | 2.4G only      | 20MHz         | Yes  | BT/BLE    | SDIO + UART    |
| CYWL6302  | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz dual  | 20MHz         | Yes  | BT/BLE    | SDIO + UART    |
| CYWL6305  | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz dual  | 20/40/80MHz   | Yes  | BT/BLE    | SDIO + UART    |
| CYWL6612  | WiFi 6 (a/b/g/n/ac/ax) | 2.4/5GHz dual  | 20MHz         | Yes  | BT/BLE    | SDIO + UART    |
| CYWL6632  | WiFi 6 (a/b/g/n/ac/ax) | 2.4/5GHz dual  | 20MHz         | Yes  | —         | SDIO/USB       |

> **Adapted (first 5 modules)**: CYWL6208 / 6209 / 6302 / 6305 / 6612 are the complete set currently supported and verified to build and run with this driver package.
> Corresponding entries exist in `Kconfig`, in `drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWLxxxx/`,
> and in the SDIO VID/PID table.
>
> Note on **CYWL6632 (not in mass production)**: a WiFi 6 dual-band **WiFi only** module, with SDIO/USB host interface.

All the modules above are based on **Infineon Technologies AG CYW43xx / CYW55xx** series WiFi + Bluetooth
combo chip solutions. The low-level drivers (WHD + BTStack) are provided by Infineon, while Aerolynq is
responsible for the RT-Thread porting layer. The hardware usage restrictions are described in [`NOTICE`](NOTICE).

---

## 2. Directory Structure

```
wifi-bt-host-driver/
├── NOTICE                              # License notice (must read)
├── LICENSE                             # Full license
├── README.md                           # This document (Chinese)
├── README_EN.md                        # This document (English)
├── RELEASE.md                          # Release notes (supported models / features / known limits)
├── Kconfig                             # Configuration menu
├── SConscript                          # Build script
├── package.json
│
├── drivers/
│   ├── wifi-host-driver/               # Infineon WHD core (Apache-2.0, upstream)
│   │   └── resources/firmware/         # WiFi firmware / CLM (Infineon PBL 1.0)
│   └── btstack-integration/            # BTStack integration layer (Apache-2.0, upstream)
│
├── External/
│   ├── README.md                       # List of components to be downloaded separately (not shipped with the package)
│   ├── btstack/                        # Infineon BTStack library (not shipped, download the latest version yourself)
│   │
│   └── wpa3-external-supplicant/       # WPA3 external supplicant (Apache-2.0, not shipped, contact Aerolynq)
│
└── Porting/
    ├── abstraction-rtos/
    │   ├── include/                    # RTOS abstraction API headers (Apache-2.0, upstream)
    │   └── source/COMPONENT_RTTHREAD/  # RT-Thread implementation (Aerolynq proprietary)
    │
    └── rt-thread/
        ├── bsp/                        # cybsp.h and other BSP headers
        ├── include/                    # Adaptation headers (cy_wcm.h / cyhal_uart.h / ...)
        │   └── hal/                    # Cypress HAL common headers (Apache-2.0, upstream)
        ├── common/                     # HAL / platform adaptation (Aerolynq proprietary)
        │   ├── cyhal_rtthread.c        # cyhal_gpio + cyhal_sdio + cybsp_init (rt_pin / mmcsd)
        │   ├── cyhal_uart.c            # HCI UART (rt_serial)
        │   ├── cybt_platform_rtthread.c
        │   ├── cybt_platform_rtthread_55x.c
        │   ├── combined_download.c     # Ymodem download of WiFi firmware + CLM
        │   ├── cy_network_buffer*.c    # LwIP network buffer adaptation
        │   └── whd_low_power.c         # WiFi low power MSH commands
        ├── src/
        │   └── aerolynq_bt_wlan.c      # WiFi + BT main entry, wlan_dev registration
        └── bt_application/             # BLE application examples
            ├── ble_app.[ch]
            ├── ble_app_cfg.c
            ├── ble_coc.[ch]
            └── ble_msh_cmd.c
```

> Note: the subdirectories `rtos_abstraction/`, `hal/` and `src/` mentioned in older versions of the README have been refactored.
> The actual directory layout is the one listed above.

---

## 3. Country Code

The driver package defaults to the **US** country code, with the following supported channel ranges:

- **2.4G**: channels 1–11
- **5G**:
  - UNII-1 (non-DFS): 36 / 40 / 44 / 48
  - UNII-2 / UNII-2 Extended (DFS, radar detection required): 52 / 56 / 60 / 64 / 100 / 104 / 108 / 112 / 116 / 120 / 124 / 128 / 132 / 136 / 140 / 144
  - UNII-3 (non-DFS): 149 / 153 / 157 / 161 / 165

> The actually available channels depend on the regulatory certification range of the module; DFS channels require radar detection to be enabled in the firmware/certification.
> For other country code files, please contact xlx@xlxwlan.com.

---

## 4. Dependencies

### 4.1 RT-Thread Side

- RT-Thread 4.1.0+ (5.0.0+ recommended)
- `RT_USING_SDIO` —— SDIO framework (required for WiFi)
- `RT_USING_WIFI` —— RT-Thread WLAN framework (required for WiFi)
- `RT_USING_SAL` —— socket abstraction layer
- `RT_USING_LWIP` —— TCP/IP stack
- Serial driver (required for Bluetooth, hardware flow control RTS/CTS must be supported)

### 4.2 External Components (Not Shipped with the Package)

| Component                   | Purpose                                | How to obtain                                     |
| --------------------------- | -------------------------------------- | ------------------------------------------------- |
| BTStack protocol stack     | Bluetooth protocol stack itself        | <https://github.com/Infineon/btstack>             |
| WPA3 external supplicant   | WPA3-SAE authentication                | Contact Aerolynq (xlx@xlxwlan.com) for the RT-Thread version |
| WLAN production test tools | RF / regulatory testing                | Contact Aerolynq (xlx@xlxwlan.com) for the RT-Thread version |

> The upstream Infineon WPA3 supplicant and production test tool chain target Infineon's own SDK and WCM components,
> so they cannot be compiled against this driver as downloaded. Aerolynq provides corresponding RT-Thread versions — please contact us.
>
> When a component is missing, the build prints a warning and skips the corresponding feature; the main WiFi functionality is not affected.
> See `External/README.md`.

---

## 5. Quick Configuration

```bash
scons --menuconfig
```

```
RT-Thread Components  →  WiFi  →  WiFi-BT Host Driver (WHD)
```

### 5.1 WiFi Configuration

```
=== WiFi Configuration ===
    Select WiFi Generation  →  WiFi 4 / WiFi 5 / WiFi 6
        Select WiFi4 Module  →  CYWL6208 / CYWL6209
        Select WiFi5 Module  →  CYWL6302 / CYWL6305
        Select WiFi6 Module  →  CYWL6612
    (PC.4)  WiFi REG_ON pin name
    (PC.5)  HOST_WAKE_IRQ pin name        # leave empty to use SDIO in-band interrupt / software polling
    [ ]     Using resources in external storage (FAL)
    [ ]     Enable Other WIFI Features    # WHD threads, WPA3, low power offload, roaming
```

Pin names use the string form supported by the RT-Thread pin framework (e.g. `"PC.4"`), which adapts to different MCUs.

### 5.2 Bluetooth Configuration

```
=== Bluetooth Configuration ===
[*] Enable Bluetooth (HCI UART)
    (uart4)   Bluetooth HCI UART device name
    (PB.9)    BT UART TX pin name
    (PB.8)    BT UART RX pin name
    (PB.14)   BT UART RTS pin name (hardware flow control)
    (PB.15)   BT UART CTS pin name
    (115200)  Baud rate for firmware download
    (3000000) Baud rate for normal operation
    (...)     BT REG_ON / BT DEV_WAKE / BT HOST_WAKE pin name
    [ ]       Enable Bluetooth sleep mode
    [ ]       Enable HCI data trace (TX/RX packets)
    [*]       Enable BTStack Library
        Bluetooth Mode     →  BLE Only / Dual Mode (Classic + BLE)
        CPU Core           →  CM0+ / CM3 / CM4 / CM7 / CM33 / CM55
        Floating Point ABI →  Hardware FPU / Software FPU
```

> **BTStack must be downloaded separately**
> This package does not contain the BTStack protocol stack itself (distributed separately by Infineon under the BTStack EULA).
> Download it from <https://github.com/Infineon/btstack> and extract it into `External/btstack/`;
> when that directory does not exist, the build automatically skips the BT sources and `Enable Bluetooth` silently has no effect.
>
> **The UART must support hardware flow control (RTS/CTS)**, otherwise the HCI firmware download stage will fail due to buffer overflow.

---

## 6. Interrupt Source Strategy (cyhal_sdio)

The SDIO adaptation in `Porting/rt-thread/common/cyhal_rtthread.c` automatically selects the card interrupt source with the following priority:

1. **OOB GPIO (best)**: enabled when `WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME` / `CYBSP_HOST_WAKE_IRQ_PIN` is configured with a valid pin.
2. **In-band SDIOIT**: receives the SDMMC driver forwarded DAT1 interrupt via RT-Thread `sdio_attach_irq`.
3. **Software polling (fallback)**: starts the `ay_sdio_poll` thread to check periodically (default 2 ms) when neither of the above is available.

Software polling and the in-band interrupt are started together; once the in-band interrupt fires for the first time, the polling thread stops automatically to save CPU.

---

## 7. Basic Usage

WiFi connection (RT-Thread WLAN API):

```c
#include <wlan_mgnt.h>

rt_wlan_scan();
rt_wlan_connect("SSID", "password");

if (rt_wlan_is_connected())
{
    rt_kprintf("WiFi connected!\n");
}
```

Common MSH commands:

```
msh> wifi scan                  # Scan
msh> wifi join SSID password    # Connect
msh> wifi status                # Status
msh> ifconfig                   # IP information
msh> ping <gateway ip>          # Test the network

# After enabling [Enable Other WIFI Features] → [Enable Low-Power Offload Commands]:
msh> wifi_ps                       # Show power save status and listen interval
msh> wifi_ps mode 2                # 0=off 1=PM1 2=PM2
msh> wifi_ps pm2_sleep 100         # PM2 sleep delay (10~2000 ms, multiples of 10)
msh> wifi_ps listen 10 dtim        # Listen interval (dtim / beacon)
msh> wifi_arp                      # ARP offload status
msh> wifi_arp enable               # Enable ARP offload
msh> wifi_tko param 10 3 5         # TKO: interval(s), retry count, retry interval(s)
msh> wifi_pf add 1 ethtype 0800     # Packet Filter (keep/discard)
```

BLE commands (after enabling BTStack):

```
msh> ble gatt start
msh> ble adv  start
msh> ble info
msh> ble help
```

---

## 8. Firmware Storage

WHD needs to download the WiFi firmware + CLM data into the chip at run time. This package supports two storage modes:

| Mode                      | Option                                      | Description                                                                  |
| ------------------------- | ------------------------------------------- | ---------------------------------------------------------------------------- |
| Internal flash array (default) | `WBHD_RESOURCES_IN_EXTERNAL_STORAGE = n` | The firmware is linked into the firmware image as a C array.               |
| External flash (FAL)      | `WBHD_RESOURCES_IN_EXTERNAL_STORAGE = y`    | A single `whd_combined` partition holds the combined "firmware + CLM" file. |

When using external flash, download the combined file with the MSH command:

```
msh> whd_combined_download             # default partition whd_combined
msh> whd_combined_download my_part     # specify the partition name
```

---

## 9. Hardware Wiring

### SDIO (WiFi)

| Signal                    | Description                  |
| ------------------------- | ---------------------------- |
| SDIO_CLK / CMD / D0–D3    | 4-bit SDIO                   |
| WL_REG_ON                 | WiFi power enable (GPIO)    |
| HOST_WAKE                 | OOB card interrupt (GPIO, optional) |

### HCI UART (Bluetooth)

The Bluetooth serial port and the MCU serial port must be **cross connected** (module TX → MCU RX, module RX → MCU TX, module CTS → MCU RTS, module RTS → MCU CTS).

| Signal                       | Description                             |
| ---------------------------- | --------------------------------------- |
| BT_UART_TX / RX              | HCI serial port (3 Mbps recommended)    |
| BT_UART_RTS / CTS            | **Hardware flow control is mandatory**  |
| BT_REG_ON                    | BT power enable (optional)              |
| BT_DEV_WAKE / BT_HOST_WAKE   | Low power wake-up (not supported yet)   |

---

## 10. Known Limitations

- **Only the SDIO bus adaptation is provided**: upstream WHD ships `whd_bus_spi_protocol.c`, but the RT-Thread porting layer in this package does not implement the SPI adaptation.
- **BTStack is not shipped with the package**: download the latest version and extract it into `External/btstack/`.
- **Interrupts depend on BSP support**: the in-band `SDIOIT` path requires the platform SDMMC driver to call `mmcsd_signal_sdio_irq()` in its ISR;
  when not implemented, it degrades to 2 ms software polling (with added latency and CPU overhead).
- **SoftAP channel width**: the driver-level `whd_wifi_set_chanspec()` targets the STA scenario, and the SoftAP start path currently uses a fixed 20MHz chanspec.
- **WPA3 depends on firmware capability**: the firmware must report `WHD_FWCAP_SAE_EXT`, otherwise a WPA3-SAE connection cannot be established;
  the external WPA3 supplicant must be obtained from Aerolynq (RT-Thread version) — see section 4.2.
- **CYWL6632 is not adapted yet**: the Kconfig option, SDIO VID/PID entry, firmware and NVRAM resources for this model
  (not in mass production) are still missing, so it cannot be selected at present.

---

## 11. Contact

- Company: 新联鑫威（杭州）科技有限公司 / Xinlian Xinwei (Hangzhou) Technology Co., Ltd. (Aerolynq)
- Website: [https://www.aerolynq.com](https://www.aerolynq.com)
- Email: xlx@xlxwlan.com

For commercial licensing, OEM/ODM cooperation, and module sample requests, please reach out using the channels above.
