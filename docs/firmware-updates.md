# Firmware updates

Inx 1.1.0 supports three update paths. The WiFi paths write the new app to the inactive OTA slot, validate the ESP32-C3 image, select that slot, and then reboot. The previous slot remains available if the new image cannot reach core startup and the bootloader must roll back.

## Web UI upload

1. On the reader, open **File Transfer → Firmware Update**.
2. The reader first tries the most recently saved WiFi network. If it cannot connect, the normal WiFi picker opens.
3. Open the `/update` URL shown on the reader.
4. Choose the release-build `firmware.bin`, confirm the warning, and select **Upload, validate, and install**.
5. Keep the reader powered until it reboots.

The browser page checks the file extension and size. The firmware then checks the ESP image magic, ESP32-C3 chip ID, inactive-slot capacity, complete byte count, and ESP-IDF image validation before changing the boot slot.

The update API uses a random server-session token. A different website cannot submit a firmware image without first reading that same-origin token.

## Online OTA

On the reader, open **Settings → Device Actions → Check for updates**, choose **Online update**, connect to WiFi, and confirm the release.

The updater checks the latest GitHub release and downloads its exact `firmware.bin` asset over certificate-validated HTTPS. Tagging a release automatically publishes that asset through `.github/workflows/release.yml`.

Release tags should be plain semantic versions such as `1.1.0`. Tags beginning with `v` are also understood by the device version comparator.

## Standard build and release process

After a requested firmware change is implemented, finish the work with this release process unless the user explicitly asks for a local-only build:

1. Document the functional build changes in the commit and release notes.
2. Bump `[inx].version` in `platformio.ini` to the requested release identifier.
3. Build the release firmware with `pio run -e gh_release`.
4. Copy `.pio/build/gh_release/firmware.bin` to `bin/firmware-<version>.bin` for the local archive.
5. Create a new branch named for the version, stage the source and generated web headers, and commit the changes.
6. Push the branch to the `fork` remote.
7. Open a pull request into `main`, merge it, and push `main`.
8. Create and push a Git tag with the same version string.
9. Confirm `.github/workflows/release.yml` publishes the GitHub Release with `firmware.bin`, `bootloader.bin`, and `partitions.bin` attached.

The tag push is the step that publishes the OTA release asset; pushing a branch alone is not enough.

## Local USB flasher

Run:

```sh
python3 scripts/flash_firmware.py
```

The script opens a native `.bin` file picker, finds the USB serial port, validates that the image targets ESP32-C3 and fits the Inx app slot, shows the complete command, and asks you to type `FLASH`.

The default safe mode writes:

| Offset | Image |
| --- | --- |
| `0x0` | Inx bootloader |
| `0x8000` | Inx partition table |
| `0xe000` | OTA selector reset to `app0` |
| `0x10000` | Selected `firmware.bin` |

Resetting the OTA selector is important after the device has previously booted from `app1`; writing only `0x10000` would otherwise leave the old slot selected.

Useful options:

```sh
# Build and flash the current v1.1.0 release
python3 scripts/flash_firmware.py --build

# Select a file in the command itself
python3 scripts/flash_firmware.py bin/firmware-1.1.0.bin

# Print and validate without writing
python3 scripts/flash_firmware.py --dry-run --port /dev/cu.usbmodem1101 bin/firmware-1.1.0.bin

# Skip the confirmation in an automated local workflow
python3 scripts/flash_firmware.py --build --port /dev/cu.usbmodem1101 --yes
```

PlatformIO already supplies `esptool`. Without PlatformIO, install it with:

```sh
python3 -m pip install esptool
```

USB-locked devices cannot use the local serial flasher. Use the built-in web upload, online OTA, SD-card update, or the supported CrossPoint unlock/recovery flow instead.

## HTTP API

- `GET /update` — update page
- `GET /api/update/status` — version, active/target slot, capacity, progress, error, and session token
- `POST /api/update/upload?size=<bytes>&token=<token>` — multipart upload with a `firmware` file

The upload response is sent before the reader waits 2.5 seconds and reboots. The server is intentionally available only while the File Transfer or Update Server screen is open.
