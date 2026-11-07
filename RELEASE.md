# Release Notes

## v1.0.0

发布日期：2026-11-05

面向 RT-Thread 的 Aerolynq® CYWL 系列 WiFi + 蓝牙 combo 模组驱动包首个正式版本。

---

## 支持的模组型号

本版本共支持 5 款模组，均已适配并验证可编译运行。

| 模组     | 芯片       | WiFi                   | 频段          | 带宽         | 蓝牙   | 主接口      |
| -------- | ---------- | ---------------------- | ------------- | ------------ | ------ | ----------- |
| CYWL6208 | CYW43438   | WiFi 4 (b/g/n)         | 2.4G 仅       | 20MHz        | BT/BLE | SDIO + UART |
| CYWL6209 | CYW43439   | WiFi 4 (b/g/n)         | 2.4G 仅       | 20MHz        | BT/BLE | SDIO + UART |
| CYWL6302 | CYW43012   | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz 双频 | 20MHz        | BT/BLE | SDIO + UART |
| CYWL6305 | CYW43455   | WiFi 5 (a/b/g/n/ac)    | 2.4/5GHz 双频 | 20/40/80MHz  | BT/BLE | SDIO + UART |
| CYWL6612 | CYW55512   | WiFi 6 (a/b/g/n/ac/ax) | 2.4/5GHz 双频 | 20MHz        | BT/BLE | SDIO + UART |

WiFi 主接口统一为 SDIO（4-bit），蓝牙主接口统一为 HCI UART。带宽为模组射频实际支持的信道宽度，受模组硬件与射频认证约束。

## 支持的功能特性

### WiFi

| 功能 | 说明 | Kconfig 选项 |
| --- | --- | --- |
| STA 联网 | 注册为 RT-Thread WLAN 设备，可直接使用 `rt_wlan_*` API 与 LwIP 联网 | 随包启用 |
| 固件下载 | 支持内部 Flash 数组与外部 Flash（FAL）两种存放方式 | `WBHD_RESOURCES_IN_EXTERNAL_STORAGE` |
| WPA3-SAE | 固件不支持wpa3的需配合外部 supplicant 组件，另见下方「外部依赖」 | `WHD_USING_WPA3` |
| 低功耗 offload | PM/PM2、ARP offload、TKO、Packet Filter、WoWL 共 5 组 MSH 命令 | `WHD_USING_LOW_POWER` |
| Roaming | 漫游相关 API | `WHD_USING_ROAM` |
| 射频产测 | MFG / RF 测试工具链 | `WHD_USING_MFG_TEST` |

### 蓝牙

| 功能 | 说明 | Kconfig 选项 |
| --- | --- | --- |
| BLE | 集成 BLE 应用示例（广播、扫描、GATT、COC） | 随包启用 |
| 经典蓝牙双模 | Classic + BLE 双模，依赖 BTStack 双模库 | `WHD_USING_BTSTACK` + `BTSTACK_MODE_DUALMODE` |
| 睡眠模式 | 控制器睡眠与唤醒引脚支持 | `BT_SLEEP_MODE_ENABLED` |
| HCI 数据跟踪 | HCI TX/RX 报文抓取，便于调试 | `CYBT_HCI_DATA_TRACE` |

### 中断源策略

SDIO 卡中断按下列优先级自动选择，无需手工配置：

1. **OOB GPIO**（最优）—— 配置了 `WBHD_BSP_HOST_WAKE_IRQ_PIN_NAME` 时启用。
2. **带内 SDIOIT** —— 通过 RT-Thread `sdio_attach_irq` 接收 SDMMC 驱动转发的 DAT1 中断。
3. **软件轮询**（兜底）—— 无上述条件时启动轮询线程（默认 2 ms）。

## 外部依赖

以下组件不随本包分发，需自行获取：

| 组件 | 用途 | 获取方式 |
| --- | --- | --- |
| BTStack 协议栈 | 蓝牙协议栈本体 | <https://github.com/Infineon/btstack> |
| WPA3 外部 supplicant | WPA3-SAE 认证 | 联系 Aerolynq（xlx@xlxwlan.com）获取 RT-Thread 版本 |
| WLAN 产测工具链 | 射频 / 法规测试 | 联系 Aerolynq（xlx@xlxwlan.com）获取 RT-Thread 版本 |

详见 `External/README.md` 与 `NOTICE` 第 6 节。

## 已知限制

- **仅提供 SDIO 总线适配**：WHD 上游自带 `whd_bus_spi_protocol.c`，但本包的 RT-Thread 移植层未实现 SPI 适配。
- **BTStack 不随包分发**：需自行下载解压到 `External/btstack/`；目录不存在时构建会打印警告并跳过蓝牙部分，WiFi 部分不受影响。
- **中断依赖 BSP 配合**：带内 `SDIOIT` 路径要求平台 SDMMC 驱动在 ISR 中调用 `mmcsd_signal_sdio_irq()`；未实现时退化为 2 ms 软件轮询（延迟增加、CPU 开销上升）。
- **SoftAP 信道宽度**：驱动层 `whd_wifi_set_chanspec()` 面向 STA 场景，SoftAP 启动路径当前固定使用 20MHz chanspec。
- **WPA3 依赖固件能力**：固件需上报 `WHD_FWCAP_SAE_EXT`，否则无法建立 WPA3-SAE 连接。
- **CYWL6632 尚未适配**：该型号（未量产）的 Kconfig 选项、SDIO VID/PID 条目、固件与 NVRAM 资源待补齐，当前无法选型使用。

## 许可

本包采用专有许可，仅限配合 Aerolynq 模组使用。完整条款见 `LICENSE`，逐文件权利归属见 `NOTICE`。
