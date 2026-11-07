# WLEDHub v0.1.0 — CYD WLED Control Center

Standalone firmware for **ESP32-2432S028R (2.8 inch 320x240 resistive-touch CYD)**.

## Capabilities
- Wi-Fi setup portal at first boot (SSID `WLEDHub-Setup`); later reconnects automatically.
- mDNS discovery of `_wled._tcp` services; manual IP fallback, remembered in NVS.
- Up to 16 controllers, current state from WLED `/json/si` (name, IP, on/off, brightness, LED count, effect, realtime state).
- Device selection from CYD or browser at `http://wledhub.local/` (use displayed IP if mDNS unavailable).
- Native WLED on/off + brightness via JSON API; independent DDP rendering to selected device.
- Three initial DDP effects: **Solid**, **Pulse**, **Chase**. Adjustable primary RGB color, speed, intensity and brightness via web dashboard; start/stop from touch screen or dashboard.
- Chunked DDP RGB24 (480 LEDs/packet, 1440 channels), port 4048, correct byte offsets and push flag only on last packet.
- Pre-stream snapshot and best-effort restore to previous WLED state when stream stops.
- Firmware OTA upload at `http://wledhub.local/update` (firmware.bin), plus USB flash initially.
- GitHub Actions compile + unit tests + downloadable firmware artifact and versioned release.

## Flash & update
1. Install VS Code + PlatformIO; connect CYD via USB and run `pio run -e cyd`, then `pio run -e cyd -t upload`.
2. On first boot connect to Wi-Fi `WLEDHub-Setup`, captive portal at 192.168.4.1; enter your 2.4 GHz network credentials.
3. Visit the IP shown on screen or `http://wledhub.local/`.
4. Open **OTA Firmware**, select the **firmware.bin** from GitHub Actions artifacts, and upload.
5. GitHub Actions: Actions → Build CYD firmware → newest successful run → artifact `wledhub-cyd-firmware`. Releases created for tags `v*`.

> The first installation must be over USB; OTA starts working after this firmware is running. The OTA endpoint is unprotected on the LAN in v0.1: use only on a trusted network.

## WLED configuration
Ensure WLED **Sync Interfaces → Realtime** reception is enabled; DDP port is 4048. This version targets linear RGB strips. DDP fills the whole device strip, not one WLED segment. RGBW strips will get RGB streamed with their dedicated white channel inactive. During DDP streaming, other WLED effects may be interrupted. Stop DDP from WLEDHub to restore the prior JSON state. If a device disconnects, it may return to a different state due to WLED timeout.

The CYD discovery is mDNS-based, so devices hidden by VLAN/multicast restrictions must be added by IP; mDNS can never guarantee finding absolutely every device.

## File layout
- `platformio.ini` hardware setup
- `src/main.cpp` interface, network discovery, JSON API, DDP animation, and OTA
- `include/ddp_packet.h` testable packet header writer
- `test/test_ddp.py` protocol tests
- `.github/workflows/build.yml` CI firmware artifact/release
