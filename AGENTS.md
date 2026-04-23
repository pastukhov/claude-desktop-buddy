# Repository Guidelines

## Project Structure & Module Organization
- `src/` contains the ESP32 firmware. `main.cpp` drives the state machine, `ble_bridge.cpp` handles BLE transport, `character.cpp` renders GIF buddies, and `buddies/` holds one ASCII species per file.
- `characters/` stores example character packs such as `characters/bufo/` with `manifest.json` and 96px-wide GIF assets.
- `tools/` contains helper scripts for flashing and asset preparation, such as `flash_character.py` and `prep_character.py`.
- `docs/` holds screenshots and local reference material. `REFERENCE.md` is the protocol contract for other devices.

## Build, Test, and Development Commands
- `pio run` builds the firmware with PlatformIO.
- `pio run -t upload` flashes the connected device over USB.
- `pio run -t erase && pio run -t upload` wipes flash before reinstalling firmware.
- `pio run -t uploadfs` uploads the filesystem image, useful after staging a character pack.
- `python3 tools/flash_character.py characters/bufo` prepares and uploads a character pack directly.
- `python3 tools/test_serial.py` and `python3 tools/test_xfer.py` are local smoke checks for serial and transfer flows.

## Coding Style & Naming Conventions
- Use 2-space indentation, UTF-8, and LF line endings.
- Keep changes small and targeted; avoid broad refactors or unrelated formatting.
- Prefer lowercase, hyphenated filenames for new tools. In `src/`, follow the existing C++ naming style with short, descriptive file names.
- JSON and TOML should stay compact and avoid trailing commas.

## Testing Guidelines
- There is no formal automated test suite. Validate changes by building with `pio run` and, when relevant, flashing to hardware.
- For asset work, verify the pack still fits the size limit and renders correctly on device.
- If you add a new utility under `tools/`, include a minimal smoke test script and keep its exit code meaningful.

## Commit & Pull Request Guidelines
- Commit messages should be imperative and scoped when helpful, for example `config: add proxy options` or `docs: clarify reset steps`.
- This repo is a reference implementation, so PRs should focus on bugs or documentation fixes rather than new features or ports.
- Include a brief purpose statement, a summary of changes, and manual verification steps. Link related issues when applicable.

## Security & Configuration Tips
- Never commit `auth.json`, real tokens, or session artifacts.
- Treat `log/` and `sessions/` as disposable local state.
- Before changing secrets or session data, confirm the need and document the rationale.
