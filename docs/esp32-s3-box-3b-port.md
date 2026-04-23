# ESP32-S3-BOX-3B Port Notes

This document records the local fork delta from the upstream M5StickC Plus
reference firmware. The BLE wire protocol in `REFERENCE.md` is unchanged.

## Hardware target

- Replaced the PlatformIO environment with `env:esp32-s3-box-3b`.
- Switched the board definition from `m5stick-c` to `esp32s3box`.
- Switched flash partitioning to `default_16MB.csv`.
- Replaced the `M5StickCPlus` dependency with LovyanGFX/TFT_eSPI display
  support plus the existing AnimatedGIF and ArduinoJson dependencies.
- Added a local board compatibility layer in `src/board_compat.*` so the
  rest of the firmware can keep using the original `M5.*` style calls.

## Display and UI

- Changed the default display geometry from the M5StickC Plus portrait
  layout to the BOX-3B 320x240 landscape panel.
- Reworked menu, pet, info, passkey, HUD, approval, and settings layouts to
  use the larger screen.
- Replaced fixed 135x240 layout assumptions with runtime `W`, `H`, and
  center coordinates where the UI depends on screen size.
- Added `src/display_compat.h` so the ASCII and GIF renderers can draw to a
  shared `DisplaySurface` abstraction instead of directly depending on
  M5StickC Plus headers.
- Updated `docs/device.jpg` to show the BOX-3B hardware running the fork.

## Input behavior

- Mapped the BOX-3B front GPIO button to primary button behavior.
- Mapped the lower red-ring touch key / touch-controller key area to the
  secondary button behavior.
- Added touch taps for the approval buttons, screen navigation, menu rows,
  settings rows, and reset confirmation rows.
- Added long-touch menu handling to mirror the original hold-A behavior.
- Removed the M5Stick power-button UX from the documented controls; screen
  sleep now lives in the menu.

## Motion and power

- Reworked face-down detection for the BOX-3B enclosure and dock angle.
- Reworked landscape clock orientation detection around the BOX-3B IMU axis
  remap.
- Added debug readouts on the device info page for IMU, clock orientation,
  USB power, RTC validity, and face-down state.
- Replaced AXP192 battery/power calls with compatibility stubs where the
  BOX-3B does not expose equivalent battery telemetry in this firmware.
- Replaced M5Stick LED handling with `LED_PIN = -1`; the BOX-3B build has no
  dedicated status LED behavior.

## Renderer changes

- Replaced direct `#include <M5StickCPlus.h>` usage in buddy, character, and
  transfer code with `board_compat.h`.
- Made buddy geometry values mutable so setup can size them for the active
  display.
- Kept all existing ASCII species and GIF character-pack protocol behavior.

## Current verification

Built successfully with:

```bash
/home/artem/platformio/.venv/bin/pio run
```

PlatformIO reported firmware size at roughly 18% flash and 24% RAM for the
`esp32-s3-box-3b` environment.
