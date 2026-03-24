# Grocy P4 ESP32 Terminal

A touch-screen inventory terminal for [Grocy](https://grocy.info) running on the **JC1060P470C** — an ESP32-P4 based board with a 10.6" 1024×600 capacitive touchscreen.

Tap a product to add or consume one unit of stock. The display wakes on motion (via Home Assistant MQTT or the onboard camera) and sleeps after inactivity.

---

## Hardware

| Component | Details |
|-----------|---------|
| SoC | ESP32-P4 (dual-core Xtensa LX7, no built-in radio) |
| WiFi/BT | ESP32-C6 co-processor (on-board, via `esp_wifi_remote`) |
| Display | 10.6" 1024×600 MIPI-DSI, JD9165 panel IC |
| Touch | GT911 capacitive controller (I2C) |
| Camera | Built-in (optional presence detection) |
| PSRAM | 32 MB (image cache + HTTP buffers) |

The ESP32-P4 includes a hardware **PPA** (Pixel Processing Accelerator) for 2D blending; LVGL scrolling runs at 60 fps.

---

## Features

- **5-column × 2-row scrollable product grid** — filtered to a single Grocy location; scroll vertically to reveal more products
- **Add / Consume toggle** — tap the toggle to switch modes; the UI themes to green (Purchase) or red (Consume)
- **Inactivity revert** — automatically returns to Consume mode after 60 seconds
- **Product images** — fetched from Grocy and cached in PSRAM; refreshed with the product list
- **Stock quantities** — displayed per product; updated optimistically on tap, corrected on refresh
- **Error feedback** — failed stock POSTs show an error banner and a per-cell overlay; the product list is force-refreshed
- **Screen wake/sleep** — triggered by Home Assistant MQTT or the onboard camera
- **MQTT telemetry** — publishes screen state, stock operations, and OTA events
- **OTA updates** — polls a JSON manifest URL and applies firmware updates automatically
- **First-boot provisioning** — on-device WiFi SSID scan + keyboard and Grocy location picker; falls back to SoftAP captive portal

---

## Architecture

### Directory layout

```
main/
  app_main.c              Entry point: init NVS, WiFi, display, start tasks
  board/                  Display (MIPI-DSI), touch (GT911), backlight init
  grocy/                  Grocy REST API client, image cache, background task
  ui/                     LVGL screens: main grid, setup wizard
  screen/                 Backlight fade on SCREEN_EVENT_WAKE / SLEEP
  wifi/                   WiFi STA + SoftAP provisioning, MQTT client
  camera/                 Optional presence detection (stub — needs driver)
  ota/                    OTA polling task + semver manifest parser
  common/                 Event bus, NVS config helpers, PSRAM allocator
```

### Core assignment

| Core | Responsibilities |
|------|-----------------|
| Core 0 | Grocy HTTP task, OTA task, camera task (optional) |
| Core 1 | LVGL rendering task |

### Data flow

```
Grocy task (core 0)
  GET /api/stock/locations/{id}/entries  (every 300 s)
  Fetch + cache product images
  xQueueOverwrite → g_product_list_queue
        ↓
UI task (core 1, 200 ms poll)
  Render 5×2 scrollable grid
  Tap → g_stock_cmd_queue
        ↓
Grocy task
  POST /api/stock/products/{id}/add|consume
  Event bus → GROCY_EVENT_STOCK_CHANGE or STOCK_POST_FAILED
        ↓
UI task
  Success: update displayed quantity
  Failure: error banner + cell overlay + force refresh
```

### Screen wake/sleep

```
MQTT message from Home Assistant   (~2 ms latency, no extra power draw)
  or
Camera frame-diff detection        (~20–30 mA continuous)
        ↓
SCREEN_EVENT_WAKE / SCREEN_EVENT_SLEEP on event bus
        ↓
screen_ctrl: backlight fade in (400 ms) / fade out (800 ms)
```

### OTA update

```
OTA task (core 0, every 3600 s)
  GET CONFIG_GROCY_OTA_MANIFEST_URL
  {"version":"1.2.0","firmware_url":"https://..."}
  Compare semver → if newer: esp_https_ota → reboot
  Publish MQTT: ota_start / ota_complete / ota_failed
```

---

## Prerequisites

- **ESP-IDF ≥ 5.3.0** — install via the [official guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/)
- **JC1060P470C board** (ESP32-P4 + 10.6" MIPI-DSI display)
- A running [Grocy](https://grocy.info) instance reachable from the device's network
- (Optional) Home Assistant with an MQTT broker for screen wake/sleep

---

## Quick start

```bash
# 1. Source ESP-IDF
. $IDF_PATH/export.sh

# 2. Set the target (once per clone)
./build.sh set-target

# 3. Configure credentials and Grocy settings
./build.sh config        # opens menuconfig → GROCY TERMINAL menu

# 4. Build and flash
./build.sh build
./build.sh flash-monitor  # flash + open serial monitor
```

`build.sh` auto-detects the serial port (`/dev/tty.usbmodem*`, `/dev/tty.usbserial-*`, or `/dev/ttyUSB*`). Pass a port explicitly as the second argument if needed:

```bash
./build.sh flash /dev/tty.usbmodem101
```

---

## Configuration

All options live under the **GROCY TERMINAL** menu in `menuconfig` (`./build.sh config`). Values entered on the device (setup wizard) are saved to NVS and take precedence over Kconfig defaults across reboots.

### Grocy

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_API_URL` | `http://grocy.local` | Base URL of your Grocy instance |
| `CONFIG_GROCY_API_KEY` | _(empty)_ | Grocy API key — enter on-device or via menuconfig |
| `CONFIG_GROCY_LOCATION_ID` | `1` | Grocy location ID to filter products; selected on first boot |
| `CONFIG_GROCY_REFRESH_INTERVAL_SEC` | `300` | How often (seconds) to re-fetch the product list (10–3600) |

### WiFi

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_WIFI_SSID` | _(empty)_ | Default SSID for initial flash |
| `CONFIG_GROCY_WIFI_PASSWORD` | _(empty)_ | Default password for initial flash |
| `CONFIG_GROCY_PROVISIONED_BY_KCONFIG` | `n` | Skip the setup wizard and use Kconfig values directly |

If WiFi credentials are absent from NVS, the device starts a **SoftAP captive portal** (`grocy-setup` SSID). Connect from any browser to enter credentials without a companion app.

### MQTT (Home Assistant integration)

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_MQTT_BROKER_URL` | `mqtt://homeassistant.local` | Broker URL; use `mqtts://` for TLS |
| `CONFIG_GROCY_MQTT_PORT` | `0` | Port override (0 = scheme default) |
| `CONFIG_GROCY_MQTT_USERNAME` | _(empty)_ | MQTT username |
| `CONFIG_GROCY_MQTT_PASSWORD` | _(empty)_ | MQTT password |
| `CONFIG_GROCY_DEVICE_ID` | _(empty)_ | MQTT topic device ID (defaults to eFuse MAC address) |
| `CONFIG_GROCY_MQTT_LOG_LEVEL` | `2` | Forward serial logs to MQTT (0=none, 4=debug) |

#### MQTT topics

All topics are rooted at `grocy_terminal/<device_id>/` where `<device_id>` is the eFuse MAC address (or `CONFIG_GROCY_DEVICE_ID` if set).

| Topic | Direction | Payload | Description |
|-------|-----------|---------|-------------|
| `grocy_terminal/<id>/status` | Publish (retained) | `online` / `offline` | Connection state; `offline` is the LWT |
| `grocy_terminal/<id>/screen/set` | Subscribe | `1` / `0` | Wake (`1`) or sleep (`0`) the screen |
| `grocy_terminal/<id>/screen` | Publish (retained) | `1` / `0` | Current screen state after wake/sleep event |
| `grocy_terminal/<id>/events` | Publish | JSON | Telemetry events (see below) |
| `grocy_terminal/<id>/log` | Publish | JSON | Forwarded serial log lines (if `CONFIG_GROCY_MQTT_LOG_LEVEL > 0`) |

**Event payloads** on `.../events` all share the envelope `{"ts":<unix>,"type":"<type>", ...}`:

| `type` | Extra fields | Trigger |
|--------|-------------|---------|
| `boot` | `version`, `reset_reason` | Device (re)started |
| `screen_wake` | `source` (`mqtt`\|`camera`) | Screen turned on |
| `screen_sleep` | `source` (`mqtt`\|`camera`) | Screen turned off |
| `stock_change` | `product_id`, `product_name`, `op` (`add`\|`consume`), `amount` | Successful stock POST |
| `product_refresh` | `count` | Product list fetched from Grocy |

**Log payloads** on `.../log`: `{"ts":<secs_since_boot>,"lvl":"I|W|E|D","tag":"<component>","msg":"<text>"}`

### OTA

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_OTA_MANIFEST_URL` | _(empty)_ | HTTPS URL to a JSON manifest; leave blank to disable OTA |
| `CONFIG_GROCY_OTA_CHECK_INTERVAL_SEC` | `3600` | OTA check interval in seconds (60–86400) |

Manifest format:
```json
{ "version": "1.2.0", "firmware_url": "https://example.com/grocy_terminal.bin" }
```

OTA uses two app partitions with automatic rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

### Screen wake source

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_SCREEN_WAKE_CAMERA` | `n` | Enable camera-based presence detection (costs ~20–30 mA) |

When disabled (default), screen wake/sleep is controlled exclusively via MQTT.

### Hardware / board

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_GROCY_BACKLIGHT_GPIO` | `26` | GPIO pin for LEDC backlight PWM (-1 = always on) |
| `CONFIG_GROCY_MIPI_DSI_PHY_LDO_CHAN` | `3` | LDO channel for MIPI DSI PHY (0 = disable) |
| `CONFIG_GROCY_IMAGE_CACHE_SIZE` | `200` | Maximum number of product images kept in PSRAM |

---

## First-boot provisioning

On first boot (or after NVS is erased), the device runs a setup wizard:

1. **WiFi** — scans for nearby SSIDs; tap to select, enter password via on-screen keyboard.
2. **Grocy URL + API key** — enter the base URL and API key.
3. **Location** — fetches locations from Grocy and shows a picker; tap to select.

Settings are saved to NVS. On subsequent boots the setup wizard is skipped.

To re-run the wizard, erase NVS:
```bash
idf.py -p /dev/tty.usbmodem101 erase-flash
```

Alternatively, set `CONFIG_GROCY_PROVISIONED_BY_KCONFIG=y` to skip the wizard permanently and always use Kconfig values.

---

## Build reference

```bash
./build.sh build              # Compile firmware
./build.sh flash [PORT]       # Flash only
./build.sh monitor [PORT]     # Serial monitor only
./build.sh flash-monitor [PORT]  # Flash then open monitor (interactive TTY)
./build.sh flash-log [PORT] [SECS]  # Flash + capture log output to stdout
./build.sh config             # Open menuconfig
./build.sh clean              # Remove build/ directory
./build.sh set-target         # Set idf target to esp32p4 (run once)
./build.sh size               # Print firmware size analysis
./build.sh test               # Build and run unit tests (Linux host)
```

---

## Partition layout

| Name | Type | Size | Purpose |
|------|------|------|---------|
| `nvs` | data/nvs | 24 KB | Runtime configuration (WiFi, Grocy, MQTT) |
| `otadata` | data/ota | 8 KB | Active OTA slot tracking |
| `phy_init` | data/phy | 4 KB | RF calibration data |
| `ota_0` | app/ota_0 | 1.875 MB | Primary firmware slot |
| `ota_1` | app/ota_1 | 1.875 MB | OTA update slot |

---

## Development notes

- **PSRAM** is used for image cache, HTTP response buffers, and LVGL frame buffers. Allocate large buffers with `PSRAM_MALLOC` / `PSRAM_CALLOC` (from `common/psram_alloc.h`).
- **Task communication** uses FreeRTOS queues (`xQueueOverwrite` for product list, depth-8 queue for stock commands) and the custom event bus (`common/event_bus.h`) for screen and Grocy lifecycle events.
- **HTTP keep-alive**: the Grocy client proactively closes idle connections after 60 s to avoid server-side RST surprises.
- **Camera presence detection** (`main/camera/cam_presence.c`) is currently a stub. A real driver needs to capture low-res grayscale frames and compute mean absolute difference between frames; the threshold and wake/sleep timers are already wired up.
- **Unit tests** live in `test_app/` and run on a Linux host target (`./build.sh test`).
