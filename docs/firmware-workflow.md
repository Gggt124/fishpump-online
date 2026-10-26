# Firmware Workflow Guide: Build, Flash & Versioning

## 1. Opening in IDE
Open the workspace using the multi-root configuration:
- In VS Code / Antigravity IDE: `File` -> `Open Workspace from File...` -> select `fishpump.code-workspace`.
- The status bar will show `⚡ Firmware (ESP32-S3)`.

## 2. Building & Flashing via ESP-IDF Extension
- Click the **Build** (gear) icon in the bottom status bar (or run `idf.py -C firmware build`).
- Click the **Flash** (lightning bolt) icon to flash to connected ESP32-S3 via UART / USB.
- Click the **Monitor** (screen) icon to inspect logs and verify version output.

## 3. Version Control Strategy
- **Development Version:** Edit `firmware/version.txt` (e.g. `0.2.0-dev`).
- **Release Tagging:** Create scoped git tags to avoid interference with frontend/infra:
  ```bash
  git tag -a fw-v0.2.0 -m "Release firmware v0.2.0"
  git push origin fw-v0.2.0
  ```
- **Checking Firmware Releases:**
  ```bash
  git describe --tags --match "fw-v*"
  ```
