# WiFi-BT Host Driver for RT-Thread

WiFi 4/5/6 and Bluetooth host driver for RT-Thread, built on Infineon's WHDand BTStack, for Aerolynq CYWL series combo modules over SDIO + HCI UART.
**License**: Proprietary (Aerolynq, hardware-restricted) + Apache-2.0 (upstream components).

面向 RT-Thread 的 Aerolynq® **CYWL 系列 WiFi + 蓝牙 combo 模组**驱动软件包：

- **WiFi**：基于 Infineon WHD，通过 SDIO 接入，注册为 RT-Thread WLAN 设备，
  可直接使用 `rt_wlan_*` API 与 LwIP 联网，支持 WiFi 4 / 5 / 6 模组。
- **蓝牙**：基于 Infineon BTStack，通过 HCI UART 接入，支持 BLE 及可选的经典蓝牙双模。

**License**: Proprietary（Aerolynq® 专有，仅限配合 Aerolynq® 模组使用）+ Apache-2.0（上游组件）。
完整条款及逐文件权利归属见 [`LICENSE`](LICENSE) / [`NOTICE`](NOTICE)。

---

## 1. 简介

支持的 Aerolynq® **CYWL 系列模组**如下：


| 模组     | WiFi                   | 频段          | 带宽         | WPA3 | 蓝牙   | 主接口      |
| -------- | ---------------------- | ------------- | ------------ | ---- | ------ | ----------- |
| CYWL6208 | WiFi 4 (b/g/n)         | 2.4G 仅       | 20MHz        | 不支持 | BT/BLE | SDIO + UART |
| CYWL6209 | WiFi 4 (b/g/n)         | 2.4G 仅       | 20MHz        | 支持 | BT/BLE | SDIO + UART |
| CYWL6302 | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz 双频 | 20MHz        | 支持 | BT/BLE | SDIO + UART |
| CYWL6305 | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz 双频 | 20/40/80MHz  | 支持 | BT/BLE | SDIO + UART |
| CYWL6612 | WiFi 6 (a/b/g/n/ac/ax) | 2.4/5GHz 双频 | 20MHz        | 支持 | BT/BLE | SDIO + UART |
| CYWL6632 | WiFi 6 (a/b/g/n/ac/ax) | 2.4/5GHz 双频 | 20MHz        | 支持 | —     | SDIO/USB     |

> **已适配（前 5 款）**：CYWL6208 / 6209 / 6302 / 6305 / 6612 是本驱动包当前支持并已验证可编译运行的完整集合，
> 在 `Kconfig`、`drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWLxxxx/`
> 与 SDIO VID/PID 表中均有对应项。
>
> 备注**CYWL6632（未量产）**：WiFi 6 双频 **WiFi only** 模组，主接口 SDIO/USB

以上模组均基于 **Infineon Technologies AG CYW43xx / CYW55xx** 系列 WiFi + 蓝牙
combo 芯片方案，底层驱动（WHD + BTStack）由 Infineon 提供，Aerolynq 负责 RT-Thread
移植层。硬件使用范围限制见 [`NOTICE`](NOTICE)。

---

## 2. 目录结构

```
wifi-bt-host-driver/
├── NOTICE                              # 授权说明（必读）
├── LICENSE                             # 完整许可
├── README.md                           # 本文件
├── RELEASE.md                          # 版本说明（支持型号 / 功能特性 / 已知限制）
├── Kconfig                             # 配置菜单
├── SConscript                          # 构建脚本
├── package.json
│
├── drivers/
│   ├── wifi-host-driver/               # Infineon WHD 核心 (Apache-2.0, 上游)
│   │   └── resources/firmware/         # WiFi 固件 / CLM (Infineon PBL 1.0)
│   └── btstack-integration/            # BTStack 集成层 (Apache-2.0, 上游)
│
├── External/
│   ├── README.md                       # 需自行下载的组件清单 (本目录内容不随包分发)
│   ├── btstack/                        # Infineon BTStack 库 (不随包分发, 需自行下载最新版本)
│   ├
│   └── wpa3-external-supplicant/       # WPA3 外部 supplicant (Apache-2.0, 不随包分发, 联系 Aerolynq 获取)
│
└── Porting/
    ├── abstraction-rtos/
    │   ├── include/                    # RTOS 抽象 API 头 (Apache-2.0, 上游)
    │   └── source/COMPONENT_RTTHREAD/  # RT-Thread 实现 (Aerolynq 专有)
    │
    └── rt-thread/
        ├── bsp/                        # cybsp.h 等 BSP 头
        ├── include/                    # 适配头文件（cy_wcm.h / cyhal_uart.h / ...）
        │   └── hal/                    # Cypress HAL 公共头 (Apache-2.0, 上游)
        ├── common/                     # HAL / 平台适配（Aerolynq 专有）
        │   ├── cyhal_rtthread.c        # cyhal_gpio + cyhal_sdio + cybsp_init (rt_pin / mmcsd)
        │   ├── cyhal_uart.c            # HCI UART (rt_serial)
        │   ├── cybt_platform_rtthread.c
        │   ├── cybt_platform_rtthread_55x.c
        │   ├── combined_download.c     # Ymodem 下载 WiFi 固件 + CLM
        │   ├── cy_network_buffer*.c    # LwIP 网络缓冲适配
        │   └── whd_low_power.c         # WiFi 低功耗 MSH 命令
        ├── src/
        │   └── aerolynq_bt_wlan.c      # WiFi + BT 主入口、wlan_dev 注册
        └── bt_application/             # BLE 应用示例
            ├── ble_app.[ch]
            ├── ble_app_cfg.c
            ├── ble_coc.[ch]
            └── ble_msh_cmd.c
```

> 注意：旧版本 README 中提到的 `rtos_abstraction/`、`hal/`、`src/` 等子目录已重构。
> 实际目录以上表为准。

---

## 3. 国家码

驱动包默认国家码为 **US**，支持的信道范围：

- **2.4G**：1–11 信道
- **5G**：
  - UNII-1（非 DFS）：36 / 40 / 44 / 48
  - UNII-2 / UNII-2 Extended（DFS，需雷达检测）：52 / 56 / 60 / 64 / 100 / 104 / 108 / 112 / 116 / 120 / 124 / 128 / 132 / 136 / 140 / 144
  - UNII-3（非 DFS）：149 / 153 / 157 / 161 / 165

> 实际可用信道以模组的射频认证范围为准，DFS 信道需确认固件/认证已启用雷达检测功能。
> 如需其他国家码文件，请联系 xlx@xlxwlan.com


## 4. 依赖

### 4.1 RT-Thread 侧

- RT-Thread 4.1.0+（推荐 5.0.0+）
- `RT_USING_SDIO` —— SDIO 框架（WiFi 必需）
- `RT_USING_WIFI` —— RT-Thread WLAN 框架（WiFi 必需）
- `RT_USING_SAL` —— socket 抽象层
- `RT_USING_LWIP` —— TCP/IP 协议栈
- 串口驱动（蓝牙必需，要求支持硬件流控 RTS/CTS）

### 4.2 外部组件（不随包分发）

| 组件 | 用途 | 获取方式 |
| --- | --- | --- |
| BTStack 协议栈 | 蓝牙协议栈本体 | <https://github.com/Infineon/btstack> |
| WPA3 外部 supplicant | WPA3-SAE 认证 | 联系 Aerolynq（xlx@xlxwlan.com）获取 RT-Thread 版本 |
| WLAN 产测工具链 | 射频 / 法规测试 | 联系 Aerolynq（xlx@xlxwlan.com）获取 RT-Thread 版本 |

> 上游 Infineon 的 WPA3 supplicant 与产测工具链均面向 Infineon 自家 SDK 与 WCM 组件，
> 直接下载无法在本驱动上编译。Aerolynq 提供了对应的 RT-Thread 版本，请联系我们获取。
>
> 组件缺失时构建会打印警告并跳过相应功能，WiFi 主功能不受影响。
> 详见 `External/README.md`。

---

## 5. 快速配置

```bash
scons --menuconfig
```

```
RT-Thread Components  →  WiFi  →  WiFi-BT Host Driver (WHD)
```

### 5.1 WiFi 配置

```
=== WiFi Configuration ===
    Select WiFi Generation  →  WiFi 4 / WiFi 5 / WiFi 6
        Select WiFi4 Module  →  CYWL6208 / CYWL6209
        Select WiFi5 Module  →  CYWL6302 / CYWL6305
        Select WiFi6 Module  →  CYWL6612
    (PC.4)  WiFi REG_ON pin name
    (PC.5)  HOST_WAKE_IRQ pin name        # 留空则走 SDIO 带内中断 / 软件轮询
    [ ]     Using resources in external storage (FAL)
    [ ]     Enable Other WIFI Features    # WHD 线程、WPA3、低功耗 offload、Roaming
```

引脚名称使用 RT-Thread 引脚框架支持的字符串形式（如 `"PC.4"`），适配多种 MCU。

### 5.2 蓝牙配置

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

> **BTStack 需自行下载**
> 本包不含 BTStack 协议栈本体（由 Infineon 单独分发，BTStack EULA）。
> 需从 <https://github.com/Infineon/btstack> 下载后解压到 `External/btstack/`；
> 该目录不存在时构建会自动跳过 BT 源码，`Enable Bluetooth` 静默失效。
>
> **UART 必须支持硬件流控（RTS/CTS）**，否则 HCI 固件下载阶段会因缓冲区溢出而失败。

---

## 6. 中断源策略（cyhal_sdio）

`Porting/rt-thread/common/cyhal_rtthread.c` 中的 SDIO 适配按下列优先级自动选择卡中断源：

1. **OOB GPIO（最优）**：当 `WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME` / `CYBSP_HOST_WAKE_IRQ_PIN` 配置了有效引脚时启用。
2. **带内 SDIOIT**：通过 RT-Thread `sdio_attach_irq` 接收 SDMMC 驱动转发的 DAT1 中断。
3. **软件轮询（兜底）**：在没有上述两种条件可用时启动 `ay_sdio_poll` 线程周期性检查（默认 2 ms）。

软件轮询与带内中断会同时启动；一旦带内中断首次触发，轮询线程会自动停止以节省 CPU。

---

## 7. 基本使用

WiFi 连接（RT-Thread WLAN API）：

```c
#include <wlan_mgnt.h>

rt_wlan_scan();
rt_wlan_connect("SSID", "password");

if (rt_wlan_is_connected())
{
    rt_kprintf("WiFi connected!\n");
}
```

常用 MSH 命令：

```
msh> wifi scan                  # 扫描
msh> wifi join SSID password    # 连接
msh> wifi status                # 状态
msh> ifconfig                   # IP 信息
msh> ping 网关ip     # 测网络

# 启用 [Enable Other WIFI Features] → [Enable Low-Power Offload Commands] 后：
msh> wifi_ps                       # 查看省电状态与监听间隔
msh> wifi_ps mode 2                # 0=off 1=PM1 2=PM2
msh> wifi_ps pm2_sleep 100         # PM2 回睡延时（10~2000 ms，10 的倍数）
msh> wifi_ps listen 10 dtim        # 监听间隔（dtim / beacon）
msh> wifi_arp                      # ARP offload 状态
msh> wifi_arp enable               # 启用 ARP offload
msh> wifi_tko param 10 3 5         # TKO: 间隔(s) 重试次数 重试间隔(s)
msh> wifi_pf add 1 ethtype 0800     # Packet Filter（keep/discard）
```

BLE 命令（启用 BTStack 后）：

```
msh> ble gatt start
msh> ble adv  start
msh> ble info
msh> ble help
```

---

## 8. 固件存储

WHD 需要在运行时将 WiFi 固件 + CLM 数据下载到芯片。本软件包支持两种存放方式：

| 模式                    | 选项                                      | 说明                                                         |
| ----------------------- | ----------------------------------------- | ------------------------------------------------------------ |
| 内部 Flash 数组（默认） | `WBHD_RESOURCES_IN_EXTERNAL_STORAGE = n` | 固件以 C 数组方式链接进 firmware 镜像。                      |
| 外部 Flash（FAL）       | `WBHD_RESOURCES_IN_EXTERNAL_STORAGE = y` | 使用单个 `whd_combined` 分区存放 “固件 + CLM” 合并文件。 |

使用外部 Flash 时，通过 MSH 命令下载合并文件：

```
msh> whd_combined_download             # 默认分区 whd_combined
msh> whd_combined_download my_part     # 指定分区名
```

---

## 9. 硬件接线

### SDIO（WiFi）

| 信号                    | 说明                     |
| ----------------------- | ------------------------ |
| SDIO_CLK / CMD / D0–D3 | 4-bit SDIO               |
| WL_REG_ON               | WiFi 上电使能（GPIO）    |
| HOST_WAKE               | OOB 卡中断（GPIO，可选） |

### HCI UART（蓝牙）

蓝牙串口与 MCU 串口需要**交叉连接**（模组 TX → MCU RX，模组 RX → MCU TX 模组 cts → MCU rts  模组 Rts → MCU cts）。

| 信号                       | 说明                                       |
| -------------------------- | ------------------------------------------ |
| BT_UART_TX / RX            | HCI 串口（建议 3 Mbps）                    |
| BT_UART_RTS / CTS          | **必须**启用硬件流控                       |
| BT_REG_ON                  | BT 上电使能（可选）                        |
| BT_DEV_WAKE / BT_HOST_WAKE | 低功耗唤醒（目前不支持）                   |

---

## 10. 已知限制

- **当前仅提供 SDIO 总线适配**：WHD 上游自带 `whd_bus_spi_protocol.c`，但本包的 RT-Thread 移植层未实现 SPI 适配。
- **BTStack 不随包分发**：需自行下载最新版本解压到 `External/btstack/`。
- **中断依赖 BSP 配合**：带内 `SDIOIT` 路径要求平台 SDMMC 驱动在 ISR 中调用 `mmcsd_signal_sdio_irq()`；
  未实现时退化为 2ms 软件轮询（有延迟增加与 CPU 开销）。
- **SoftAP 信道宽度**：驱动层 `whd_wifi_set_chanspec()` 面向 STA 场景，SoftAP 启动路径当前固定使用 20MHz chanspec。
- **WPA3 依赖固件能力**：固件需上报 `WHD_FWCAP_SAE_EXT`，否则无法建立 WPA3-SAE 连接；
  外部 WPA3 supplicant 需联系 Aerolynq 获取 RT-Thread 版本（见 4.2 节）。
- **CYWL6632 尚未适配**：该型号（未量产）的 Kconfig 选项、SDIO VID/PID 条目、
  固件与 NVRAM 资源待补齐，当前无法选型使用。

---

## 11. 联系方式

- 公司：新联鑫威（杭州）科技有限公司 / Xinlian Xinwei (Hangzhou) Technology Co., Ltd. （Aerolynq）
- 官网：[https://www.aerolynq.com](https://www.aerolynq.com)
- 邮件：xlx@xlxwlan.com

商业授权、OEM/ODM 合作、模组样品申请，请通过上述方式与我们联系。
