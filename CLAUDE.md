# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A touch-screen inventory terminal for [Grocy](https://grocy.info) running on the **JC1060P470C** (ESP32-P4 based, 10.6" 1024×600 capacitive touchscreen). The UI shows a toggle (add/subtract mode) and a **5-column × 2-row scrollable grid** of products filtered to a configured Grocy **location**. Scrolling reveals additional rows of products. Tapping a product increments or decrements its stock via the Grocy REST API. Screen wake/sleep is triggered by Home Assistant (MQTT) or the onboard camera. OTA updates are supported.

## Hardware

- **SoC**: ESP32-P4 (dual-core Xtensa LX7, no built-in radio — requires ESP32-C6 co-processor for WiFi/BT on this board)
- **Display**: 10.6" 1024×600, connected via MIPI-DSI
- **Touch**: Capacitive (GT911 or similar I2C controller)
- **Camera**: Built into JC1060P470C (used for presence detection as an alternative to HA motion sensor)

The ESP32-P4 has a hardware **PPA** (Pixel Processing Accelerator) for 2D blending/fill and 32 MB PSRAM. LVGL scrolling runs at 60 fps without software pagination.

## Framework & Toolchain

This project uses **ESP-IDF** (not Arduino/PlatformIO). The UI layer uses **LVGL**.

### Setup

```bash
# Source ESP-IDF environment (adjust path to your idf installation)
. $IDF_PATH/export.sh

# Set target (must be done once per clone)
idf.py set-target esp32p4
```

### Build & Flash

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
idf.py -p /dev/ttyUSB0 flash monitor   # flash + open serial monitor
```

### Configuration

```bash
idf.py menuconfig   # opens Kconfig-based configuration UI
```

All runtime-tunable parameters (Grocy URL, API key, location ID, refresh interval, OTA URL, etc.) are exposed via Kconfig under a `GROCY TERMINAL` menu and stored in NVS for persistence across reboots.

**WiFi credentials** and **Grocy API key** are configurable both via `menuconfig` (Kconfig, for developer initial flash) and via the on-device setup screen (LVGL keyboard + SSID scan list on first boot). NVS values take precedence over Kconfig defaults. If provisioning fails or NVS is cleared, the device falls back to SoftAP captive portal mode (`wifi_provisioning` component, SoftAP transport) so credentials can be entered from any browser without a companion app.

### Single-component build (faster iteration)

```bash
idf.py build --component <component_name>
```

## Architecture

```
main/
  app_main.c          # Entry point: initialises NVS, WiFi, display, touch, then starts tasks
  grocy/              # Grocy API client (HTTP/HTTPS via esp_http_client)
    grocy_client.c/h  # fetch_location_products(), post_stock_entry()
  ui/                 # LVGL-based UI, runs on display task
    ui_main.c/h       # Root screen: toggle widget + scrollable 5×2 product grid
    ui_product_btn.c/h# Individual product button widget
    ui_setup.c/h      # First-boot setup screen: WiFi SSID list + keyboard, location picker
  screen/             # Display & backlight power management
    screen_ctrl.c/h   # screen_on(), screen_off(), triggered by MQTT or camera task
  camera/             # Optional presence detection via onboard camera
    cam_presence.c/h  # Publishes wake/sleep events to internal event bus
  ota/                # OTA firmware update via HTTPS
    ota_task.c/h      # Polls configurable URL for manifest; applies update
  common/
    event_bus.h       # esp_event loop IDs shared across components
    config.h          # Kconfig-derived constants
components/           # Optional: third-party LVGL component or board BSP
  lvgl/
  esp_lcd_*/          # ESP-IDF LCD + touch driver wrappers
```

### Data flow

1. **WiFi** connects on boot (credentials from NVS; falls back to SoftAP provisioning if absent).
2. **Setup screen** runs on first boot to collect WiFi credentials and select the Grocy location. Location ID is saved to NVS.
3. Grocy client calls `GET /api/stock/locations/{locationId}/entries` to fetch products for the configured location, at startup and on the configured refresh interval (`GROCY_REFRESH_INTERVAL_SEC`).
4. **LVGL task** renders the 5-column scrollable grid; product data is passed via a FreeRTOS queue to keep the HTTP and UI tasks decoupled.
5. **Tap event** → UI posts a `GROCY_STOCK_CHANGE` event on the event bus → grocy task sends `POST /api/stock/products/{id}/add` or `.../consume`.
6. **Screen power**: a dedicated task subscribes to `SCREEN_WAKE` / `SCREEN_SLEEP` events posted by either the MQTT subscriber (Home Assistant) or the camera presence detector.
7. **OTA task** wakes periodically, fetches a JSON manifest from the configured URL, compares versions, and applies the update via `esp_https_ota` if a newer build is available.

### Grocy location filtering

Products are scoped to a single Grocy location (storage area). During the setup screen, `GET /api/objects/locations` populates a picker; the selected `locationId` is stored in NVS as `GROCY_LOCATION_ID`. The main product fetch uses `GET /api/stock/locations/{locationId}/entries` — only products currently stocked there are shown. Products are sorted alphabetically by name.

Each product button displays:
- Product picture fetched via `GET /api/files/productpictures/{filename}` (field `picture_file_name` on the product object). Images are cached in PSRAM and re-fetched only on product list refresh.
- Product name
- Current stock quantity + unit

Tapping always adds or subtracts **1 unit** depending on the toggle state.

### Screen wake strategy (energy)

The camera-based presence detection avoids the need for a separate PIR sensor and network round-trip, but costs ~20–30 mA continuously. The HA MQTT trigger adds ~2 ms latency and relies on WiFi staying connected but costs no extra current. **Recommended default**: HA MQTT trigger; camera detection as an opt-in fallback (guarded by `CONFIG_GROCY_SCREEN_WAKE_CAMERA` Kconfig bool).

### OTA

Uses `esp_https_ota`. The OTA task fetches `CONFIG_GROCY_OTA_MANIFEST_URL` (a JSON file containing `version` and `firmware_url`). On success, reboots into the new image. Rollback is handled automatically by ESP-IDF's app partition scheme (requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

## Key ESP-IDF configuration flags (sdkconfig)

| Key | Purpose |
|-----|---------|
| `CONFIG_GROCY_API_URL` | Base URL of Grocy instance |
| `CONFIG_GROCY_API_KEY` | Grocy API key (store in NVS, not in sdkconfig for production) |
| `CONFIG_GROCY_LOCATION_ID` | Grocy location ID to filter displayed products |
| `CONFIG_GROCY_REFRESH_INTERVAL_SEC` | Product list refresh cadence |
| `CONFIG_GROCY_OTA_MANIFEST_URL` | HTTPS URL to OTA manifest JSON |
| `CONFIG_GROCY_SCREEN_WAKE_SOURCE` | `MQTT` or `CAMERA` |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | Must be `y` for safe OTA |
| `CONFIG_PARTITION_TABLE_TWO_OTA` | Required two-OTA-slot partition table |
