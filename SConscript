# WiFi-BT-Host-Driver SConscript for RT-Thread
# Supports CYWL modules: WiFi4/WiFi5/WiFi6

Import('RTT_ROOT')
Import('rtconfig')
from building import *

import os
import sys

cwd = GetCurrentDir()

# The package is built only when it is selected in menuconfig.
if GetDepend(['PKG_USING_WIFI_BT_HOST_DRIVER']) == False:
    objs = []
    Return('objs')

# ============================================================================
# WiFi Host Driver Core Source Files
# ============================================================================
src = []

# Bus protocol files (SDIO)
src += Glob('drivers/wifi-host-driver/src/bus_protocols/whd_bus.c')
src += Glob('drivers/wifi-host-driver/src/bus_protocols/whd_bus_common.c')
src += Glob('drivers/wifi-host-driver/src/bus_protocols/whd_bus_sdio_protocol.c')
src += Glob('drivers/wifi-host-driver/src/bus_protocols/whd_bus_m2m_protocol.c')
src += Glob('drivers/wifi-host-driver/src/bus_protocols/whd_bus_spi_protocol.c')

# WHD core source files
src += Glob('drivers/wifi-host-driver/src/whd_ap.c')
src += Glob('drivers/wifi-host-driver/src/whd_buffer_api.c')
src += Glob('drivers/wifi-host-driver/src/whd_cdc_bdc.c')
src += Glob('drivers/wifi-host-driver/src/whd_chip.c')
src += Glob('drivers/wifi-host-driver/src/whd_chip_constants.c')
src += Glob('drivers/wifi-host-driver/src/whd_clm.c')
src += Glob('drivers/wifi-host-driver/src/whd_commonring.c')
src += Glob('drivers/wifi-host-driver/src/whd_debug.c')
src += Glob('drivers/wifi-host-driver/src/whd_events.c')
src += Glob('drivers/wifi-host-driver/src/whd_flowring.c')
src += Glob('drivers/wifi-host-driver/src/whd_hal_port.c')
src += Glob('drivers/wifi-host-driver/src/whd_logging.c')
src += Glob('drivers/wifi-host-driver/src/whd_management.c')
src += Glob('drivers/wifi-host-driver/src/whd_msgbuf_txrx.c')
src += Glob('drivers/wifi-host-driver/src/whd_network_if.c')
src += Glob('drivers/wifi-host-driver/src/whd_proto.c')
src += Glob('drivers/wifi-host-driver/src/whd_resource_if.c')
src += Glob('drivers/wifi-host-driver/src/whd_ring.c')
src += Glob('drivers/wifi-host-driver/src/whd_sdpcm.c')
src += Glob('drivers/wifi-host-driver/src/whd_thread.c')
src += Glob('drivers/wifi-host-driver/src/whd_utils.c')
src += Glob('drivers/wifi-host-driver/src/whd_wifi.c')
src += Glob('drivers/wifi-host-driver/src/whd_wifi_api.c')
src += Glob('drivers/wifi-host-driver/src/whd_wifi_p2p.c')

# WiFi Roaming APIs (optional - enable via WHD_USING_ROAM in Kconfig)
if GetDepend(['WHD_USING_ROAM']):
    src += Glob('drivers/wifi-host-driver/src/whd_roam.c')

# ============================================================================
# RTOS Abstraction Layer (abstraction-rtos with RT-Thread component)
# ============================================================================
src += Glob('Porting/abstraction-rtos/source/COMPONENT_RTTHREAD/cyabs_rtos_dsram.c')
src += Glob('Porting/abstraction-rtos/source/COMPONENT_RTTHREAD/cyabs_rtos_rtthread.c')
src += Glob('Porting/abstraction-rtos/source/cy_worker_thread.c')

# ============================================================================
# RT-Thread Porting Layer
# ============================================================================
# HAL and BSP source files
src += Glob('Porting/rt-thread/common/cyhal_rtthread.c')
src += Glob('Porting/rt-thread/common/cy_network_buffer.c')

# WLAN source files
src += Glob('Porting/rt-thread/src/aerolynq_bt_wlan.c')

# ============================================================================
# Low-Power Offload (Optional)
# ============================================================================
# wifi_ps / wifi_arp / wifi_tko / wifi_pf / wifi_wowl MSH commands
if GetDepend(['WHD_USING_LOW_POWER']):
    src += Glob('Porting/rt-thread/common/whd_low_power.c')

# ============================================================================
# Include Paths
# ============================================================================
path = []

# WHD driver headers
path += [cwd + '/drivers/wifi-host-driver/inc']
path += [cwd + '/drivers/wifi-host-driver/src']
path += [cwd + '/drivers/wifi-host-driver/src/include']
path += [cwd + '/drivers/wifi-host-driver/src/bus_protocols']
path += [cwd + '/drivers/wifi-host-driver/resources/resource_imp']

# RT-Thread porting headers
path += [cwd + '/Porting/rt-thread/include']
path += [cwd + '/Porting/rt-thread/include/bsp']
path += [cwd + '/Porting/rt-thread/include/hal']

# RTOS abstraction layer headers (multi-RTOS support, RT-Thread component)
path += [cwd + '/Porting/abstraction-rtos/include']
path += [cwd + '/Porting/abstraction-rtos/include/COMPONENT_RTTHREAD']

# ============================================================================
# Resource Files (Firmware, CLM, NVRAM)
# ============================================================================

# Resource implementation, selected by the storage mode.
# External storage (FAL): the combined firmware file is read from a single
#   FAL partition at runtime.
# External storage (file system): the combined firmware file is read from a
#   file on a mounted file system (SD card, USB disk) at runtime.
# Internal storage: the combined firmware file is linked into the image.
# All firmware uses the combined format with XOR obfuscation.

# The firmware is not linked into the image when it is read from external
# storage, in either flavour.
wbhd_resources_external = GetDepend(['WBHD_RESOURCES_EXTERNAL_STORAGE'])

if wbhd_resources_external:
    src += Glob('drivers/wifi-host-driver/resources/resource_imp/whd_resources_combined_external.c')
    if GetDepend(['WBHD_RESOURCES_IN_EXTERNAL_STORAGE']):
        src += Glob('Porting/rt-thread/common/combined_download.c')
else:
    src += Glob('drivers/wifi-host-driver/resources/resource_imp/whd_resources_combined.c')

# ============================================================================
# Per-Module Firmware and NVRAM
# ============================================================================
# Each module contributes its firmware blob and its NVRAM directory. When the
# manufacturing test mode is enabled the MFG firmware is used instead of the
# normal one. The NVRAM directory is omitted when a custom NVRAM file is
# supplied by the application.

# CYWL6208 (CYW43438) - WiFi 4
if GetDepend(['WHD_USING_CYWL6208']):
    if not wbhd_resources_external:
        if GetDepend(['WHD_USING_MFG_TEST']):
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6208/wifi_cywl6208_mfg_firmware.c')
        else:
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6208/wifi_cywl6208_firmware.c')
    if GetDepend(['WHD_RESOURCES_CUSTOM_NVRAM']) == False:
        path += [cwd + '/drivers/wifi-host-driver/resources/nvram/COMPONENT_CYWL6208']

# CYWL6209 (CYW43439) - WiFi 4
if GetDepend(['WHD_USING_CYWL6209']):
    if not wbhd_resources_external:
        if GetDepend(['WHD_USING_MFG_TEST']):
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6209/wifi_cywl6209_mfg_firmware.c')
        else:
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6209/wifi_cywl6209_firmware.c')
    if GetDepend(['WHD_RESOURCES_CUSTOM_NVRAM']) == False:
        path += [cwd + '/drivers/wifi-host-driver/resources/nvram/COMPONENT_CYWL6209']

# CYWL6302 (CYW43012) - WiFi 5
if GetDepend(['WHD_USING_CYWL6302']):
    if not wbhd_resources_external:
        if GetDepend(['WHD_USING_MFG_TEST']):
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6302/wifi_cywl6302_mfg_firmware.c')
        else:
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6302/wifi_cywl6302_firmware.c')
    if GetDepend(['WHD_RESOURCES_CUSTOM_NVRAM']) == False:
        path += [cwd + '/drivers/wifi-host-driver/resources/nvram/COMPONENT_CYWL6302']

# CYWL6305 (CYW43455) - WiFi 5
if GetDepend(['WHD_USING_CYWL6305']):
    if not wbhd_resources_external:
        if GetDepend(['WHD_USING_MFG_TEST']):
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6305/wifi_cywl6305_mfg_firmware.c')
        else:
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6305/wifi_cywl6305_firmware.c')
    if GetDepend(['WHD_RESOURCES_CUSTOM_NVRAM']) == False:
        path += [cwd + '/drivers/wifi-host-driver/resources/nvram/COMPONENT_CYWL6305']

# CYWL6612 (CYW55512) - WiFi 6
if GetDepend(['WHD_USING_CYWL6612']):
    if not wbhd_resources_external:
        if GetDepend(['WHD_USING_MFG_TEST']):
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6612/wifi_cywl6612_mfg_firmware.c')
        else:
            src += Glob('drivers/wifi-host-driver/resources/firmware/COMPONENT_CYWL6612/wifi_cywl6612_firmware.c')
    if GetDepend(['WHD_RESOURCES_CUSTOM_NVRAM']) == False:
        path += [cwd + '/drivers/wifi-host-driver/resources/nvram/COMPONENT_CYWL6612']

# ============================================================================
# Bluetooth Support (BTstack)
# ============================================================================
# The BTStack protocol stack library is distributed separately by Infineon
# under the Infineon End User License Agreement and is not part of this
# package. It is obtained from https://github.com/Infineon/btstack and placed
# under External/btstack/ in this package directory.
_btstack_dir = os.path.join(cwd, 'External', 'btstack')
_btstack_available = os.path.isdir(_btstack_dir)

if GetDepend(['WHD_USING_BLUETOOTH']) and not _btstack_available:
    print('Warning: WHD_USING_BLUETOOTH is enabled but the BTStack protocol')
    print('  stack is not present at: ' + _btstack_dir)
    print('  The Bluetooth sources are skipped for this build.')
    print('  Obtain Latest version BTStack from https://github.com/Infineon/btstack and')
    print('  place it under External/btstack/ inside this package.')

if GetDepend(['WHD_USING_BLUETOOTH']) and _btstack_available:
    # BTstack common sources
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_hci_rx_task.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_hci_tx_task.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_host_stack_platform_interface.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_platform_main.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_platform_task.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_platform_trace.c')
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_serialize.c')
    
    # BLE application
    src += Glob('Porting/rt-thread/bt_application/ble_app.c')
    src += Glob('Porting/rt-thread/bt_application/ble_app_cfg.c')
    src += Glob('Porting/rt-thread/bt_application/ble_coc.c')
    src += Glob('Porting/rt-thread/bt_application/ble_msh_cmd.c')
    
    # BLE application headers
    path += [cwd + '/Porting/rt-thread/bt_application']

    # The 55x series uses its own patchram download implementation.
    if GetDepend(['WBHD_USING_WIFI6']):
        src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_patchram_download_55x.c')
        src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_prm_55x.c')
    else:
        src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_patchram_download.c')
        src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/common/cybt_prm.c')
    
    # Platform HAL wrapper
    src += Glob('drivers/btstack-integration/COMPONENT_HCI-UART/platform_hal_wrappers/platform_hal_wrapper.c')
    
    # RT-Thread platform implementation - UART HAL
    src += Glob('Porting/rt-thread/common/cyhal_uart.c')
    
    # RT-Thread platform layer. The 55x series uses the platform file with
    # the Enter Download Mode support.
    if GetDepend(['WBHD_USING_WIFI6']):
        src += Glob('Porting/rt-thread/common/cybt_platform_rtthread_55x.c')
    else:
        src += Glob('Porting/rt-thread/common/cybt_platform_rtthread.c')
    
    # MCU-specific BT platform diagnostics. Enable the file matching the MCU.
    # src += Glob('Porting/platform_mcu/cybt_platform_diag_gd32f4xx.c')
    # src += Glob('Porting/platform_mcu/cybt_platform_diag_stm32f4xx.c')
    
    # BTstack include paths
    path += [cwd + '/drivers/btstack-integration/COMPONENT_HCI-UART/include']
    path += [cwd + '/drivers/btstack-integration/COMPONENT_HCI-UART/common']
    path += [cwd + '/drivers/btstack-integration/COMPONENT_HCI-UART/platform_hal_wrappers']
    
    # Bluetooth controller firmware patch data.
    #
    # Each chip directory holds the .hcd patch data and fw_version.txt. The
    # C file that the BTStack integration links against (btfw.c, holding
    # brcm_patch_version, brcm_patchram_format, brcm_patchram_buf and
    # brcm_patch_ram_length) is generated from the .hcd at build time by
    # tools/hcd2c.py. Only the binary .hcd is distributed; the generated C
    # file is excluded from version control (see .gitignore in the bt-fw
    # directory).
    #
    # A chip whose directory has no .hcd is skipped with a warning.
    _bt_fw_root = cwd + '/drivers/btstack-integration/bt-fw'
    _bt_fw_tools = _bt_fw_root + '/tools/hcd2c.py'

    # The generator is imported so that the .c file exists when this
    # SConscript is read. The IDE project generators (Keil/IAR/Eclipse) only
    # read the SConscript and never run a build, so a generated file declared
    # with Env.Command alone would not exist yet when the project is
    # generated. Generating at load time covers the scons build, the
    # --target=mdk5/iar/eclipse generators and the generated IDE project.
    #
    # The file is regenerated below only when the .hcd is newer than it.
    sys.path.insert(0, _bt_fw_tools.replace('/hcd2c.py', ''))
    import hcd2c

    def _add_bt_fw(chip_dir):
        """Generate btfw.c from the .hcd patch data of one chip.

        Returns the build nodes to link (empty list when skipped).
        The chip include path is appended to ``path`` as a side effect.
        """
        _hcd = os.path.join(_bt_fw_root, chip_dir, 'btfw.hcd')
        if not os.path.isfile(_hcd):
            print('Warning: Bluetooth firmware patch data not found: ' + _hcd)
            print('  The Bluetooth firmware download is skipped for this build.')
            return []

        _version_file = os.path.join(_bt_fw_root, chip_dir, 'fw_version.txt')
        _version = ''
        if os.path.isfile(_version_file):
            with open(_version_file, 'r') as _f:
                _version = _f.read().strip()

        _gen_c = os.path.join(_bt_fw_root, chip_dir, 'btfw.c')

        # Remove the file name used before the rename to btfw.c, which
        # matches the name of the Infineon reference file.
        _legacy = os.path.join(_bt_fw_root, chip_dir, 'btfw_gen.c')
        if os.path.isfile(_legacy):
            try:
                os.remove(_legacy)
            except OSError as _e:
                print('Warning: cannot remove legacy generated file '
                      + _legacy + ': ' + str(_e))

        # Regenerate when the .c file is missing or older than its inputs.
        _regen = (not os.path.isfile(_gen_c)
                  or os.path.getmtime(_hcd) > os.path.getmtime(_gen_c)
                  or (os.path.isfile(_version_file)
                      and os.path.getmtime(_version_file) > os.path.getmtime(_gen_c)))
        if _regen:
            hcd2c.generate(_hcd, _gen_c, _version)

        # Declare the generation as a build step as well, so that touching
        # the .hcd rebuilds the .c when scons runs a real build.
        # The --version option is only passed when a version string is
        # actually available; passing it with an empty value makes the
        # argparse based tool fail with "expected one argument".
        _gen_cmd = sys.executable + ' ' + _bt_fw_tools + ' "$SOURCE" "$TARGET"'
        if _version:
            _gen_cmd += ' --version $BT_FW_VERSION'
        _gen = Env.Command(
            _gen_c,
            [_hcd, _bt_fw_tools],
            _gen_cmd,
            ENV={'PATH': os.environ.get('PATH', '')},
            BT_FW_VERSION=_version)
        path.append(os.path.join(_bt_fw_root, chip_dir))
        return _gen

    # CYWL6208 (CYW43438)
    if GetDepend(['WHD_USING_CYWL6208']):
        src.extend(_add_bt_fw('bt-fw-cywl-6208'))

    # CYWL6209 (CYW43439)
    if GetDepend(['WHD_USING_CYWL6209']):
        src.extend(_add_bt_fw('bt-fw-cywl-6209'))

    # CYWL6302 (CYW43012)
    if GetDepend(['WHD_USING_CYWL6302']):
        src.extend(_add_bt_fw('bt-fw-cywl-6302'))

    # CYWL6305 (CYW43455)
    if GetDepend(['WHD_USING_CYWL6305']):
        src.extend(_add_bt_fw('bt-fw-cywl-6305'))

    # CYWL6612 (CYW55512) - WiFi6
    if GetDepend(['WHD_USING_CYWL6612']):
        src.extend(_add_bt_fw('bt-fw-cywl-6612'))

# ============================================================================
# WPA3 External Supplicant (Optional)
# ============================================================================
# Not distributed with this package. Obtain the RT-Thread port of the
# component from Aerolynq (xlx@xlxwlan.com) and place it under
# External/wpa3-external-supplicant/.
_wpa3_dir = os.path.join(cwd, 'External', 'wpa3-external-supplicant')

if GetDepend(['WHD_USING_WPA3']):
    if not os.path.isdir(_wpa3_dir):
        print('Warning: WHD_USING_WPA3 is enabled but the WPA3 external')
        print('  supplicant is not present at: ' + _wpa3_dir)
        print('  Its sources are skipped for this build.')
        print('  Obtain the RT-Thread port of the component from Aerolynq')
        print('  (xlx@xlxwlan.com) and place it under')
        print('  External/wpa3-external-supplicant/.')
    else:
        src += Glob('External/wpa3-external-supplicant/source/wpa3_ext_supp.c')
        src += Glob('External/wpa3-external-supplicant/source/wpa3_ext_crypto.c')
        src += Glob('External/wpa3-external-supplicant/source/wpa3_ext_utils.c')
        path += [cwd + '/External/wpa3-external-supplicant/include']

# ============================================================================
# WiFi MFG Test (Optional)
# ============================================================================
# The manufacturing / RF test tooling is not distributed with this package.
# The upstream Infineon build does not fit this driver, so Aerolynq provides
# an RT-Thread port of it; obtain it from Aerolynq (xlx@xlxwlan.com) and
# place it under External/wifi-mfg-test/.
_mfg_test_dir = os.path.join(cwd, 'External', 'wifi-mfg-test')

if GetDepend(['WHD_USING_MFG_TEST']):
    if not os.path.isdir(_mfg_test_dir):
        print('Warning: WHD_USING_MFG_TEST is enabled but the manufacturing')
        print('  test tooling is not present at: ' + _mfg_test_dir)
        print('  The RF / regulatory test sources are skipped for this build.')
        print('  Obtain the RT-Thread port of the tooling from Aerolynq')
        print('  (xlx@xlxwlan.com) and place it under External/wifi-mfg-test/.')
    else:
        src += Glob('External/wifi-mfg-test/source/mfg_test_common.c')
        # The WCM component ioctl interface is used on RT-Thread.
        src += Glob('External/wifi-mfg-test/target/COMPONENT_WCM/mfg_test_comp_ioctl.c')
        path += [cwd + '/External/wifi-mfg-test/include']
        path += [cwd + '/External/wifi-mfg-test/source']

# ============================================================================
# Build Group
# ============================================================================
CPPDEFINES = []
LIBS = []
LIBPATH = []

# The XMC7000 data cache helpers do not apply to the MCUs supported here.
CPPDEFINES += ['CY_DISABLE_XMC7000_DATA_CACHE']

# The CYW55xxx series uses the BLHS (Bootloader Handshake) protocol for the
# firmware download.
if GetDepend(['WBHD_USING_WIFI6']):
    CPPDEFINES += ['BLHS_SUPPORT']

# WHD_USING_WPA3, WPA3_EXT_LOG_ENABLE and WPA3_EXT_HEX_LOG_ENABLE are
# already defined in rtconfig.h by Kconfig and are not repeated here.

# The HAL type definitions are required by the Bluetooth sources only.
if GetDepend(['WHD_USING_BLUETOOTH']) and _btstack_available:
    CPPDEFINES += ['CYHAL_API_VERSION=2']
    CPPDEFINES += ['CY_USING_HAL']

# Anonymous unions and structs are handled in cy_result.h:
#   - AC5: #pragma anon_unions
#   - AC6: native support with GNU extensions
LOCAL_CFLAGS = ''

# ============================================================================
# BTStack Prebuilt Library (Optional)
# ============================================================================
if GetDepend(['WHD_USING_BTSTACK']) and _btstack_available:
    # BTStack common include
    path += [cwd + '/External/btstack/wiced_include']

    # Select Bluetooth mode (BLE or Dual Mode)
    if GetDepend(['BTSTACK_MODE_DUALMODE']):
        bt_mode = 'COMPONENT_WICED_DUALMODE'
    else:
        bt_mode = 'COMPONENT_WICED_BLE'

    # Select CPU core based on Kconfig
    if GetDepend(['BTSTACK_CPU_CM0P']):
        cpu_core = 'COMPONENT_CM0P'
    elif GetDepend(['BTSTACK_CPU_CM3']):
        cpu_core = 'COMPONENT_CM3'
    elif GetDepend(['BTSTACK_CPU_CM7']):
        cpu_core = 'COMPONENT_CM7'
    elif GetDepend(['BTSTACK_CPU_CM33']):
        cpu_core = 'COMPONENT_CM33'
    elif GetDepend(['BTSTACK_CPU_CM55']):
        cpu_core = 'COMPONENT_CM55'
    else:
        cpu_core = 'COMPONENT_CM4'

    # Select floating point ABI
    if GetDepend(['BTSTACK_FPU_SOFTFP']):
        fpu_mode = 'COMPONENT_SOFTFP'
    else:
        fpu_mode = 'COMPONENT_HARDFP'

    # Library selection by toolchain
    if rtconfig.PLATFORM in ['armcc', 'armclang']:
        toolchain = 'TOOLCHAIN_ARM'
        lib_file = 'libbtstack.ar'
    elif rtconfig.PLATFORM in ['iar']:
        toolchain = 'TOOLCHAIN_IAR'
        lib_file = 'libbtstack.a'
    else:
        toolchain = 'TOOLCHAIN_GCC_ARM'
        lib_file = 'libbtstack.a'

    lib_path = os.path.normpath(os.path.join(
        cwd, 'External', 'btstack', 'stack', bt_mode, cpu_core, fpu_mode, toolchain))
    lib_full_path = os.path.join(lib_path, lib_file)

    if os.path.exists(lib_full_path):
        LIBPATH += [lib_path]
        # The library name is passed without the prefix and the extension.
        lib_name = os.path.splitext(lib_file)[0]
        if lib_name.startswith('lib'):
            lib_name = lib_name[3:]
        LIBS += [lib_name]

        # The Keil project generator does not recognise the .ar library
        # format, so the .uvprojx file is patched to set FileType=4 on the
        # library entry.
        if rtconfig.PLATFORM in ['armcc', 'armclang']:
            import atexit
            import glob
            import xml.etree.ElementTree as ET

            # Store in closure for atexit callback
            _btstack_lib_path = lib_full_path
            _btstack_cwd = cwd

            def _patch_keil_uvprojx_add_btstack_ar():
                """Add the BTStack library to the Keil .uvprojx files.

                The project files are searched for in the directories above
                the SConscript and in the scons root directory.
                """
                import glob
                import xml.etree.ElementTree as ET

                if not os.path.exists(_btstack_lib_path):
                    return

                search_patterns = []

                # Project file in the parent directories of the SConscript.
                for i in range(1, 6):
                    parent = os.path.normpath(os.path.join(_btstack_cwd, *(['..'] * i)))
                    search_patterns.append(os.path.join(parent, '*.uvprojx'))

                # Project file in the scons root directory.
                try:
                    scons_root = Dir('#').abspath
                    search_patterns.append(os.path.join(scons_root, '*.uvprojx'))
                    search_patterns.append(os.path.join(scons_root, 'projects', '*.uvprojx'))
                    search_patterns.append(os.path.join(scons_root, 'projects', '*', '*.uvprojx'))
                except:
                    pass

                project_files = set()
                for pattern in search_patterns:
                    try:
                        project_files.update(glob.glob(pattern))
                    except:
                        pass

                if not project_files:
                    return

                for project_file in project_files:
                    try:
                        _patch_single_uvprojx(project_file, _btstack_lib_path)
                    except Exception:
                        pass

            def _patch_single_uvprojx(project_file, lib_src):
                """Patch a single .uvprojx file to include BTStack library."""
                import xml.etree.ElementTree as ET

                tree = ET.parse(project_file)
                root = tree.getroot()

                groups = root.find('Targets/Target/Groups')
                if groups is None:
                    return

                # Find wifi-bt-host-driver group
                target_group = None
                for grp in groups.findall('Group'):
                    gn = grp.find('GroupName')
                    if gn is not None and gn.text and 'rtt-wifi-bt-host-driver' in gn.text:
                        target_group = grp
                        break

                if target_group is None:
                    return

                files_node = target_group.find('Files')
                if files_node is None:
                    files_node = ET.SubElement(target_group, 'Files')

                # Calculate relative path from project file to library
                project_dir = os.path.dirname(os.path.abspath(project_file))
                rel_path = os.path.relpath(lib_src, project_dir)
                rel_path = rel_path.replace('/', '\\')

                # Check if library already exists in project
                lib_exists = False
                for fnode in files_node.findall('File'):
                    fp = fnode.find('FilePath')
                    fn = fnode.find('FileName')
                    if fp is not None and fp.text:
                        # The library entry is identified by its path or its file name.
                        if 'libbtstack' in fp.text.lower():
                            ft = fnode.find('FileType')
                            if ft is None:
                                ft = ET.SubElement(fnode, 'FileType')
                            ft.text = '4'
                            fp.text = rel_path
                            if fn is None:
                                fn = ET.SubElement(fnode, 'FileName')
                            fn.text = os.path.basename(lib_src)
                            lib_exists = True
                            break

                # The library entry is added when it is not present yet.
                if not lib_exists:
                    file_node = ET.SubElement(files_node, 'File')
                    fn = ET.SubElement(file_node, 'FileName')
                    fn.text = os.path.basename(lib_src)
                    ft = ET.SubElement(file_node, 'FileType')
                    ft.text = '4'
                    fp = ET.SubElement(file_node, 'FilePath')
                    fp.text = rel_path

                tree.write(project_file, encoding='UTF-8', xml_declaration=True)

            # Register the patch function to run after SCons finishes
            atexit.register(_patch_keil_uvprojx_add_btstack_ar)
    else:
        print('Warning: BTStack library not found at: ' + lib_full_path)

group = DefineGroup('rtt-wifi-bt-host-driver', src, depend = ['PKG_USING_WIFI_BT_HOST_DRIVER'], CPPPATH = path, CPPDEFINES = CPPDEFINES, LOCAL_CFLAGS = LOCAL_CFLAGS, LIBS = LIBS, LIBPATH = LIBPATH)

Return('group')
