# External/ - components that are NOT part of this package

The contents of this directory are **not** distributed with the
`wifi-bt-host-driver` package. Each of them is published separately by its
owner, under its own licence, and must be obtained from that owner.

| Path | Component | Owner | Licence | Where to get it |
|---|---|---|---|---|
| `btstack/` | Infineon BTStack protocol stack (library + headers) | Infineon | Infineon End User License Agreement | <https://github.com/Infineon/btstack> (release 4.1.4.23695, the same version this package was built against) |
| `wpa3-external-supplicant/` | WPA3 external supplicant | Infineon | Infineon End User License Agreement | **Ask Aerolynq** - see below |
| `wifi-mfg-test/` | WLAN manufacturing / RF test tooling | Infineon | Infineon End User License Agreement | **Ask Aerolynq** - see below |

The Infineon End User License Agreement was originally issued by Cypress
Semiconductor Corporation, which Infineon acquired in 2019; the current owner
of these components and the issuer of the agreement is Infineon
Technologies AG.

### `wpa3-external-supplicant/`: obtain it from Aerolynq

The upstream Infineon `wpa3-external-supplicant` component is built for the
Infineon Wi-Fi connection manager (WCM) and for Infineon's own SDK layout,
not for this driver. Downloading it from Infineon and dropping it into
`External/wpa3-external-supplicant/` will not build here.

Aerolynq therefore provides an **RT-Thread port of the WPA3 external
supplicant**, patched for this driver and for the RT-Thread WCM shim.
Obtain it from Aerolynq:

- Contact: xlx@xlxwlan.com
- It is provided under the Apache License, Version 2.0, because it is
  derived from the Infineon component.

The WPA3 external supplicant is needed to establish a WPA3-SAE connection.
It is only required when the `WHD_USING_WPA3` option is enabled; the rest of
the package builds and works without it.

### `wifi-mfg-test/`: obtain it from Aerolynq, not from Infineon

The upstream Infineon `wifi-mfg-test` component is **not** usable as-is for an
RT-Thread project: it is built for the Infineon Wi-Fi connection manager
(WCM) and for Infineon's own SDK layout, not for this driver. Downloading it
from Infineon and dropping it into `External/wifi-mfg-test/` will not build
here.

Aerolynq therefore provides an **RT-Thread port of the manufacturing test
tooling**, adapted to this driver and to the RT-Thread WCM shim. Obtain it
from Aerolynq:

- Contact: xlx@xlxwlan.com
- It is provided under the Infineon End User License Agreement, because it is
  derived from the Infineon component.

The manufacturing test tooling is used for RF and regulatory validation of
the finished product (conducted output power, spectrum, receiver sensitivity
and the regulatory measurements your product certification requires). It is
only needed if you intend to run those measurements yourself; for a normal
Wi-Fi product it is not required, and the rest of the package builds and works
without it.

## How to use them

Place the component under the matching path in this directory, for example:

```
External/btstack/LICENSE.txt
External/btstack/README.md
External/btstack/stack/COMPONENT_WICED_BLE/COMPONENT_CM7/COMPONENT_SOFTFP/TOOLCHAIN_ARM/libbtstack.a
```

Then enable the matching option in `menuconfig`:

- `WHD_USING_BLUETOOTH` - the Bluetooth host stack
- `WHD_USING_BTSTACK` - link the pre-built BTStack library

If `External/btstack/` is missing, the build prints a warning and skips the
Bluetooth part; the Wi-Fi part still builds normally.

**Note:** the Bluetooth controller firmware patch data is *not* in this
directory. It is shipped inside the package as a binary `.hcd` file under
`drivers/btstack-integration/bt-fw/`, and the C file the stack links against
is generated from it at build time. See `NOTICE`, Section 4.3.

## Licence

Nothing in this directory is licensed by Aerolynq. You must obtain each
component from its owner, accept its licence terms and comply with them.
See `NOTICE`, Section 6, and `LICENSE`, Section 7.2.
