# USB Bluetooth PAN on WiCAN PRO

This branch adds an experimental PAN User (PANU) client for a phone's Bluetooth
tethering service (NAP). It requires an external Bluetooth Classic USB
controller. The PRO's ESP32-S3 radio still provides the existing onboard BLE
diagnostic connections; PAN uses a separate host and controller.

The integration is based on `dev-v6`, commit
`a628a989d310aa2a47576c80ddd779ed9c88bfb0` (`v6.00p_alfa-04`). No hardware
validation is implied by a successful firmware build or host test.

## Supported Controllers

The initial USB driver accepts full-speed standard HCI controllers with USB ID
`0a12:0001`, interface class/subclass/protocol `e0/01/01`, an interrupt IN
endpoint and bulk IN/OUT endpoints. This covers the two supplied candidates:

| Adapter | Reported controller | Pairing |
| --- | --- | --- |
| Konig CSR8510 A10 | Bluetooth 4.0, CSR manufacturer 10, HCI/LMP firmware `0x22bb` | Numeric comparison |
| DSE | Bluetooth 2.0 + EDR, CSR manufacturer 10, HCI/LMP firmware `0x0c5c` | Legacy PIN, explicitly enabled |

These are hardware candidates, not a list of adapters proven on this firmware.
USB IDs and product names can be copied by clones. In particular, the Konig's
USB revision `0x8891` occurs in Linux's CSR clone notes. Controllers requiring
firmware uploads (for example, Realtek) are not supported by this implementation.
The isochronous voice interface is unused.

## Dependency Licence

The optional PAN backend uses [BTstack v1.8.2](https://github.com/bluekitchen/btstack/tree/v1.8.2),
pinned to `075a0780f0fad7ff67d58ac19f46e8953656a752`. Its
[licence](https://github.com/bluekitchen/btstack/blob/v1.8.2/LICENSE) includes
a non-commercial-use condition. The backend is consequently excluded from
ordinary builds and BTstack is not vendored into this repository.

Before distributing combined firmware or proposing the backend upstream,
resolve those dependency terms with the project maintainers and BlueKitchen,
or replace the backend with a suitable alternative. A public source fork and
permission to redistribute combined firmware are separate considerations.
The integration, UI, policy tests and API do not change BTstack's licence.

## Build

Use ESP-IDF **6.0.2** and the normal MeatPi components dependency. The development
build was prepared with meatpi-components commit
`d8cd667e4da19813977f3466423ecc93dc84b650`.

Configure a separate build directory and settings file so an existing build is
not changed:

```sh
idf.py -B build-pan \
  -D SDKCONFIG=sdkconfig.pan.local \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-pan' build
```

The PAN-enabled configure downloads the pinned BTstack source into the build
directory. For an offline build, set `WICAN_BTSTACK_PATH` to a checkout of that
commit before configuring. Ordinary builds need neither that checkout nor a
BTstack download.

`enabled` defaults to false even in a PAN-enabled binary: no PAN worker,
controller or network interface is started until the setting is enabled.

## First Connection

1. On the WiCAN, enable **USB > Settings > USB host**, using the **host** role.
2. Under **Settings > Bluetooth > USB Bluetooth PAN**, enable PAN and submit
   the settings. Keep the WiFi access point available for pairing and recovery.
3. After the restart, attach the Bluetooth dongle using the appropriate USB
   host/OTG adapter. The USB-C connector is shared with the console/flash path.
4. Open the phone's Bluetooth settings and enable its Bluetooth tethering or
   Personal Hotspot service. Phone/carrier support still determines whether a
   NAP service and mobile Internet are available.
5. Select **Scan phones**, select the phone's Classic Bluetooth address, and
   submit the settings. Pairing uses the boot-applied address after the restart.
6. Select **Pair phone**. For the Konig, compare the six-digit code with the
   phone and select **Confirm code** only when they match. Approve the pairing
   on the phone too. For the DSE, enable legacy pairing and configure the same
   PIN on both sides before pairing.
7. The state progresses through service discovery, PAN connection and DHCP.
   **Connected** means the phone supplied an IP lease. It does not prove that
   mobile Internet, DNS or the configured MQTT/HTTPS destination is reachable.

The saved bond is scoped to the USB controller's Bluetooth address and the
configured phone. Reboots retain it. Exchanging adapters usually requires a new
pairing on that adapter. **Forget phone** removes the local bond and leaves
automatic reconnection paused; remove the WiCAN entry on the phone as well
before pairing again. Bonds are stored separately in NVS, not in settings
backups; restoring settings onto another WiCAN requires fresh pairing.

WiFi remains the preferred route by default. **Prefer phone Internet over WiFi**
raises PAN's route priority; the default route falls back when PAN goes down.
The build option selects ESP-IDF's per-interface DNS support, so the resolver
follows the selected default interface instead of retaining another uplink's DNS.
MQTT users who want publishing through PAN should select their existing MQTT
**Any network** connection policy instead of **WiFi station only**.

## Settings And Actions

Settings are registered under `usb_bluetooth_pan` and use the existing
reboot-to-apply settings, validation, persistence and backup services.

| Setting | Default | Purpose |
| --- | --- | --- |
| `enabled` | false | Start the PAN component |
| `peer_address` | empty | The only phone eligible for pairing and PAN |
| `device_name` | WiCAN PRO PAN | Name presented by the external controller |
| `auto_connect` | true | Reconnect using a saved bond |
| `prefer_pan` | false | Prefer PAN over the WiFi station |
| `reconnect_min_s`, `reconnect_max_s` | 5, 60 | Bounded exponential retry delay |
| `connect_timeout_s` | 30 | Service discovery/connection deadline |
| `dhcp_timeout_s` | 30 | Lease acquisition deadline |
| `pairing_timeout_s` | 120 | User-opened pairing window |
| `allow_legacy_pairing` | false | Permit old PIN-based controllers |
| `legacy_pin_password` | empty | PIN for explicitly enabled legacy pairing |
| `cli` | true | Register the `pan` console command |

Commands and pairing approval are live actions; they do not modify the
boot-applied settings. Manual disconnect pauses automatic reconnect until
**Connect**, **Pair phone**, or a reboot. Numeric confirmation is never
automatically accepted. Unknown phones and expired pairing windows are rejected.
The PIN uses the existing `_password` secret convention: it is masked in the
form, redacted in settings GET responses and omitted from redacted backups.
As with other firmware secrets, an empty PUT value preserves the stored value.
Keep non-redacted backups private.

## API And Console

- `GET /api/usb/pan`: runtime state, adapter identification, active phone,
  bond/pairing state, IPv4 lease, retry countdown, discovered devices and frame
  counters. Link keys and the legacy PIN are not included.
- `POST /api/usb/pan/action`: queue a live action. Body:
  `{"action":"scan"}`, `{"action":"pair"}`, `{"action":"connect"}`,
  `{"action":"disconnect"}`, `{"action":"forget"}`,
  `{"action":"reject"}` or `{"action":"confirm","value":123456}`.
- `GET/PUT /api/settings/usb_bluetooth_pan`: standard settings service.
- `GET /api/settings/usb_bluetooth_pan/schema`: field schema and defaults.
- Console: `pan status`, `pan scan`, `pan pair`, `pan connect`,
  `pan disconnect`, `pan confirm 123456`, `pan reject`, `pan forget`.

An accepted action returns HTTP 202. State is observed through GET; accepting
a command is not confirmation that the connection succeeded. Bad requests return
400, unavailable actions return 409. The existing firmware HTTP admission gate
also applies to these endpoints.

## Validation

```sh
bash tools/testbench/usb/pan_policy_test.sh
WICAN_BTSTACK_PATH=/path/to/btstack bash tools/testbench/usb/pan_stack_lifecycle_test.sh
node --check components/usb_bluetooth_pan/web/usb-pan.js
node tools/webui_preview/probe_usb_pan.mjs
```

The policy test uses ASan/UBSan to exercise address validation, foreign-peer and
expired-window rejection, retry saturation and fragmented/coalesced HCI event
and ACL reads, including full-size packets split into 1..64-byte reads. The
optional BTstack smoke test exercises 32 initialization/teardown cycles while
HCI Reset is pending, with ASan/UBSan. It models stack lifecycle, not USB hardware.
The UI test uses the existing app and mocks to exercise discovery,
staged settings, explicit numeric confirmation, connection controls and safe
rendering of phone names. Its dependencies are in
`tools/webui_preview/package.json`; `WICAN_WEB_TEST_MODULES` may point to an
external `node_modules` directory.

To test the actual generated/minified firmware page, run the probe with
`WICAN_PAN_MINIFIED_INPUT=build-pan/esp-idf/usb_bluetooth_pan/pan_index.html.gz`.
Setting `WICAN_PAN_PREVIEW_OUTPUT=/tmp/wican-pan-ui-preview.html` also creates a
standalone mock preview. Run `node tools/webui_preview/check_usb_pan_layout.mjs`
against that preview after installing the package's Playwright Chromium browser.
The layout check covers 1440x1000 and 390x844 viewports and captures screenshots.

### Recorded Software Checks

On 2026-10-15, using the dependency commits above:

- ESP-IDF 6.0.2 builds succeeded with the PAN backend enabled and disabled.
  The enabled image leaves about 24% of the existing application partition free.
- Policy/reassembly and BTstack lifecycle tests passed under ASan/UBSan.
- JavaScript syntax and UI interaction checks passed for source and generated
  minified HTML, including explicit pairing approval, secret input, manual
  disconnect/reconnect, disabled-backend controls and safe phone-name rendering.
- Playwright Chromium checks passed at desktop and mobile sizes, including
  confirmation controls staying inside their container.
- The compiler reports a 32-byte frame for the Ethernet transmit callback;
  packet-sized temporaries and frame queue storage use external RAM.

These are build and mocked/host checks, not confirmation of an Internet
connection through either supplied dongle. Firmware remains experimental until
the hardware checks below pass. Runtime internal-RAM headroom is not measured by
the application-partition size check.

Before treating the feature as ready for use or upstream contribution, test:

1. Adapter present at boot, attach/remove/reinsert, repeated cycles, controller
   initialization and USB transport faults without a crash or stale network bit.
2. Correct, wrong and expired numeric confirmations; foreign-phone pairing
   attempts; DSE legacy PIN success/failure; forget on both devices.
3. Bond reconnect after a WiCAN reboot and after a phone reboot; tethering
   disabled, out of range, scan while disconnected and manual disconnect.
4. DHCP, DNS and an actual HTTPS/MQTT request with the WiFi station disconnected.
   Check a failed lease and loss of the phone's cellular Internet separately.
5. WiFi/PAN preference and fallback, VPN traffic, sleep/wake and simultaneous
   onboard BLE diagnostics. Measure internal-RAM headroom with the full stack;
   the existing lean WiFi RAM profile can free headroom.
6. A sustained transfer that fragments Ethernet frames into HCI ACL packets;
   check dropped-frame counters, queues and unplug during a transfer.

## Contribution Layout

The feature implementation is under `components/usb_bluetooth_pan`. The firmware
composition root wires its init/start/HTTP/ordered shutdown. The UI extension is
injected while generating the embedded app, so the independently maintained
meatpi-components checkout is not modified. For upstream review, the extension
can be moved into that project's native Bluetooth page; the firmware feature and
tests remain independently reviewable here.
