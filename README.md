# Mini-E-Reader (Waveshare ESP32-S3-ePaper-1.54G)

[中文文档](README_ZH.md)

An ultra-low-power, pocket-sized E-Reader built for the **Waveshare ESP32-S3-ePaper-1.54G** development board, featuring a 1.54-inch 4-color e-paper display, UTF-8 `.txt` book pagination, intelligent deep sleep, and wireless book syncing via a **Web Bluetooth Progressive Web App (PWA)**.

<img src="assets/ESP32-S3-ePaper-1.54G-1.jpg" alt="Waveshare ESP32-S3-ePaper-1.54G" width="450">

---

## Key Features

- **1.54" 4-Color E-Paper Display (200x200)**: Clean rendering using `Font16` (22 chars/line, 8 lines) or `Font12` (30 chars/line, 11 lines) with battery indicator, book title, and reading percentage.
- **UTF-8 Word-Wrapping & Rolling Page Cache**: Multi-byte boundary protection with a 16-deep rolling offset history for instant backward and forward page turning.
- **Ultra-Low Power & Deep Sleep**: Draws $< 30\ \mu\text{A}$ in deep sleep. Latches board power rail via GPIO 17 (`gpio_hold_en`), cuts e-paper rail via GPIO 6, and wakes instantly on button press (`Ext1`).
- **Web Bluetooth PWA Sync (`/pwa`)**: Zero app installation required. Connect from any browser with Web Bluetooth (Chrome, Edge, Bluefy) to stream `.txt` books directly to `/sdcard/books/` over 512-byte MTU BLE GATT.
- **Reading State Persistence**: Automatically persists reading progress and bookmarks in `/sdcard/.reader/state.json`.

---

## Repository Structure

```text
.
├── reader_firmware/               # Production ESP-IDF Mini E-Reader firmware
│   ├── CMakeLists.txt             # Project CMake build definition
│   ├── partitions.csv             # 8MB Flash partition layout
│   ├── sdkconfig.defaults         # Octal PSRAM, NimBLE, and FATFS configurations
│   ├── main/                      # UI state machine, event dispatcher, view renderers
│   └── components/
│       ├── epaper/                # Hardware SPI display driver, fonts, GUI_Paint
│       ├── sdcard/                # 1-line SDMMC host FatFS mount at /sdcard
│       ├── button/                # Multi-button event handler (BOOT & PWR)
│       ├── power_mgr/             # GPIO 17 latch, ADC battery monitor, deep sleep
│       ├── reader_core/           # Text pager, UTF-8 parser, book library & bookmarks
│       └── ble_sync/              # NimBLE GATT service for high-speed file transfer
├── pwa/                           # Progressive Web App for Web Bluetooth book syncing
│   ├── index.html                 # Modern responsive dashboard & upload UI
│   ├── app.js                     # Web Bluetooth GATT client (0xfe60 profile)
│   ├── style.css                  # Dark/light theme styles
│   ├── manifest.json              # PWA manifest
│   ├── sw.js                      # Offline service worker
│   └── icon.svg                   # Device vector icon
├── sdcard_template/               # Initial SD card folder structure and sample books
│   └── books/
│       ├── alice_in_wonderland.txt
│       └── pride_and_prejudice.txt
├── Example/                       # Reference examples (ESP-IDF, Arduino, XiaoZhi AI)
├── Firmware/                      # Factory test binary (01_factory.bin)
└── Hardware/                      # Schematics and mechanical drawings
```

---

## 2-Button Interaction Guide

The board features two hardware buttons: **BOOT (GPIO 0)** and **PWR (GPIO 18)**.

| Screen Context | Action | Button | Function |
| :--- | :--- | :--- | :--- |
| **Reading View** | Single Click | **BOOT (GPIO 0)** | **Next Page** |
| | Single Click | **PWR (GPIO 18)** | **Previous Page** |
| | Double Click | **BOOT (GPIO 0)** | **Toggle Bookmark** |
| | Long Press (>1.2s) | **BOOT (GPIO 0)** | **Open Reader Menu** |
| | Long Press (>1.2s) | **PWR (GPIO 18)** | **Deep Sleep / Power Off** |
| **Menu View** | Single Click | **BOOT (GPIO 0)** | **Move Selection Down** ($\downarrow$) |
| | Single Click | **PWR (GPIO 18)** | **Confirm Selection** ($\checkmark$) |
| | Long Press (>1.2s) | **BOOT (GPIO 0)** | **Back to Reading View** |
| **Book Library** | Single Click | **BOOT (GPIO 0)** | **Next Book** |
| | Single Click | **PWR (GPIO 18)** | **Open Book** |
| | Long Press (>1.2s) | **BOOT (GPIO 0)** | **Back to Menu** |
| **BLE Sync View** | Long Press (>1.2s) | **BOOT (GPIO 0)** | **Exit Sync & Return to Reading** |

*Note: In Reading View, after 60 seconds of inactivity, the reader automatically persists the current byte offset and enters deep sleep to preserve battery.*

---

## Quick Start & Build Instructions

### 1. Hardware Preparation
1. Format a MicroSD card as FAT32.
2. Copy the contents of `sdcard_template/` (`books/` directory with `.txt` files) to the root of the MicroSD card.
3. Insert the card into the MicroSD slot on the ESP32-S3-ePaper board.

### 2. Building & Flashing Firmware
Prerequisites: ESP-IDF v5.2 or later configured in your environment.

```bash
cd reader_firmware

# Target ESP32-S3
idf.py set-target esp32s3

# Build firmware
idf.py build

# Flash and open serial monitor (replace /dev/ttyUSB0 with your device port)
idf.py -p /dev/ttyUSB0 flash monitor
```

### 3. Syncing Books Wirelessly via Web Bluetooth PWA
1. On the reader, open the menu (long press **BOOT**) and select **Sync Books (BLE)**.
2. Open `pwa/index.html` in any browser supporting Web Bluetooth (Google Chrome, Microsoft Edge, or Bluefy on iOS). You can also serve it via:
   ```bash
   cd pwa && npx serve
   ```
3. Click **Connect E-Reader** and pair with `Mini-E-Reader`.
4. Drag and drop `.txt` files or click to upload. Files are written directly to `/sdcard/books/` at high speed (~40 KB/s).
5. Manage existing books or delete them remotely directly from the web interface.
6. Long press **BOOT** on the device to exit sync mode and start reading!

---

## Hardware Pinout Reference

| Pin Name | GPIO | Function |
| :--- | :--- | :--- |
| `EPD_MOSI` / `SCK` / `CS` / `DC` | 13, 12, 11, 10 | SPI2 Host E-Paper Display |
| `EPD_RST` / `BUSY` / `PWR` | 9, 8, 6 | Display reset, busy line, and power switch |
| `SD_CLK` / `SD_CMD` / `SD_D0` | 39, 41, 40 | 1-line SDMMC MicroSD card interface |
| `BOOT_KEY` / `PWR_KEY` | 0, 18 | User input buttons (Ext1 wakeups) |
| `POWER_HOLD` | 17 | Board power latch (must be kept HIGH) |
| `BATTERY_ADC` | 1 | Battery voltage monitoring via ADC1_CH0 |
