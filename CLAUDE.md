# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32-S3-BOX-3B firmware for a "desk buddy" that connects to Claude desktop apps over BLE. It displays session state, permission prompts, and animated pets. This is a fork of the upstream M5StickC Plus reference — the BLE protocol is unchanged, but the hardware layer, screen layout, and input handling are rewritten for the BOX-3B.

## Build and flash

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/). Single target: `esp32-s3-box-3b`.

PlatformIO установлен в virtualenv проекта: `.venv/bin/pio`. Используй именно этот путь — `pio` в системном PATH отсутствует.

```bash
.venv/bin/pio run                          # build only
.venv/bin/pio run -t upload                # flash firmware over USB
.venv/bin/pio run -t erase && .venv/bin/pio run -t upload  # wipe flash first (use after layout/partition changes)
.venv/bin/pio run -t uploadfs              # upload LittleFS filesystem image (character packs)
```

Character pack tools (requires Python 3 venv in `.venv/`):

```bash
python3 tools/flash_character.py characters/bufo   # stage + upload character pack via USB
python3 tools/prep_character.py <source_dir>        # resize GIFs to 96px-wide normalized set
```

No automated test suite. Validate by building (`pio run`) and flashing to hardware.

## Architecture

The firmware is a single-threaded Arduino `loop()` state machine in `src/main.cpp` (~1400 lines). Key modules:

- **`board_compat.*`** — Hardware abstraction for ESP32-S3-BOX-3B (display via LovyanGFX, capacitive touch via I2C TT21100, IMU via QMI8658, backlight via LEDC PWM). Exposes a global `M5` object mimicking the M5StickC API so the rest of the code stays portable.
- **`ble_bridge.*`** — Nordic UART Service (NUS) over BLE with LE Secure Connections bonding. Line-buffered TX/RX. Advertises as `Claude-XXXX`.
- **`data.h`** — JSON parsing of heartbeat snapshots from the desktop app into `TamaState`. Also handles demo mode, time sync, and connection timeout logic. All inline/static in the header.
- **`stats.h`** — NVS-backed persistent state: approval counts, token velocity, level, species selection, owner name, settings.
- **`character.h` / `character.cpp`** — GIF character rendering using AnimatedGIF library from LittleFS.
- **`xfer.h`** — Folder-push receiver: handles the `char_begin`/`file`/`chunk`/`file_end`/`char_end` BLE protocol for streaming character packs.
- **`buddy.cpp` + `buddies/`** — 18 ASCII species, each with 7 animation functions (sleep, idle, busy, attention, celebrate, dizzy, heart). One `.cpp` per species.
- **`ui_cyr_font.h`** — Cyrillic bitmap font for the Russian-language UI.

## State machine

Seven persona states drive both ASCII and GIF rendering: `P_SLEEP`, `P_IDLE`, `P_BUSY`, `P_ATTENTION`, `P_CELEBRATE`, `P_DIZZY`, `P_HEART`. Base state is derived from BLE data (`TamaState`); one-shot states (celebrate, dizzy, heart) overlay temporarily.

## Wire protocol

`REFERENCE.md` is the stable contract. The device receives newline-delimited JSON heartbeats and commands, and sends back JSON acks/permission decisions. Do not change the protocol without updating `REFERENCE.md`.

## Conventions

- 2-space indentation, UTF-8, LF line endings.
- UI strings are in Russian (see `stateNames[]` in main.cpp, `ui_cyr_font.h`).
- Commit messages: imperative, scoped prefix when helpful (e.g., `ui: translate interface to Russian`).
- This is a reference implementation — PRs should fix bugs or docs, not add features or ports (see CONTRIBUTING.md).
