# TrailCamHub Deployment Package

This archive contains the firmware images for the BLE/Wi-Fi proof of concept. `BUILD_INFO.txt` names the board: `esp32s3` packages are built for the Waveshare ESP32-S3-A7670E-4G board (16 MB flash, 8 MB PSRAM), `esp32p4` packages for the Waveshare ESP32-P4-NANO (see [ESP32-P4-NANO](#esp32-p4-nano) below). The sections up to that one describe the ESP32-S3 package.

## Browser installer

For the latest `main` build, use the hosted [TrailCamHub browser installer](https://thomaspohl.github.io/TrailCamHub/). It requires a desktop Chromium-based browser with Web Serial support and an HTTPS connection. Connect the board with a USB data cable, choose a build (development or release), select its serial port, and confirm the browser's prompt. After installation completes, connect to the serial console on the same page to view ESP-IDF output at 115200 baud.

The page offers two builds: the **development** image with verbose (INFO) serial logging, and the **release** image with size-optimized, warnings-only logging. Both are the same firmware; pick development to watch the protocol in the serial console.

The installer page is deployed by the **Firmware Package** GitHub Actions workflow. It updates automatically on every push to `main`, and can also be published from any branch on demand: open the repository's **Actions → Firmware Package → Run workflow**, pick the branch, and run it. That build (both images) is then published to the public installer without merging, so the whole flash cycle can stay in the browser. Pull requests build a package but never update the public installer. The default CI firmware sends the `BT_KEY_ON` Wi-Fi-enable payload after BLE discovery.

## Flash the merged image

Connect the board over USB, identify its serial port, and flash the image you
want — `trailcamhub-esp32s3-dev.bin` (verbose logs) or
`trailcamhub-esp32s3-release.bin` (release):

```sh
esptool --chip esp32s3 --port PORT --baud 460800 write-flash 0x0 trailcamhub-esp32s3-dev.bin
```

This is the esptool v5 command syntax; older esptool releases name the script `esptool.py` and the command `write_flash`.

Replace `PORT` with the board's serial port. On Linux this is commonly `/dev/ttyACM0` or `/dev/ttyUSB0`; on macOS it is commonly `/dev/cu.usbmodem...`; on Windows it is a `COM` port.

The merged image includes the bootloader, partition table, initial OTA data, and application. The separate images (`bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`, `app.bin`) and their offsets in `flasher_args.json` are included for workflows that flash components individually. Verify `SHA256SUMS` before flashing.

## PoC behavior

The default CI build scans for a camera advertising the name `TC100_ED3B` or service UUID `0x8801` and connects only to that camera. It sends ASCII `BT_KEY_ON` (`42 54 5F 4B 45 59 5F 4F 4E` in hex) only if the camera's service `0x8801` has writable characteristic `0x0002` and notification/indication characteristic `0x0003`. The payload can be changed or disabled in `menuconfig`. Use this firmware only with a camera you are authorized to access.

After the camera returns Wi-Fi credentials over BLE, the firmware joins the camera's access point and runs the experimental UDP discovery, PRE_LOGIN, and ARTEMIS exchange described in the project README. It decrypts the login response, logs the session token, and requests the media list (cmdId 768), logging the decrypted response. Media downloads are not implemented yet.

At boot the firmware logs the reset reason, the available PSRAM, and memory statistics, and mounts a FAT-formatted microSD card at `/sdcard` if one is inserted. The card is never formatted by the firmware. The partition table provides two 4 MB OTA slots and a core dump partition; OTA updates themselves are not implemented yet. The image requires the board's 16 MB flash and octal PSRAM; on hardware without PSRAM it stops during boot.

The firmware logs status over its serial console. Use an authorized camera and review its status before leaving the device unattended.

## ESP32-P4-NANO

The `trailcamhub-esp32p4` package contains the same firmware built for the Waveshare ESP32-P4-NANO. It is not offered by the browser installer. Flash the merged image over the board's USB-to-UART port:

```sh
esptool --chip esp32p4 --port PORT --baud 460800 write-flash 0x0 trailcamhub-esp32p4-dev.bin
```

On this board Wi-Fi and BLE are provided by the on-board ESP32-C6 through ESP-Hosted; the image contains a matching ESP-Hosted firmware for the C6 and installs it at the first boot if the C6 runs a different version (the board restarts once; do not power it off while the log shows `Updating C6`). At boot the firmware also starts the wired Ethernet port and requests an address over DHCP; Ethernet remains the default route while the camera access point is joined. The microSD card is used in 4-bit mode. Media uploads are not implemented yet. The image runs on ESP32-P4 chip revision v1.x only; the bootloader rejects other revisions (the boot log shows `chip revision`).
