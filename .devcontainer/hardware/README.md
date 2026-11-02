# Dev container with USB boards

`devcontainer.json` in this directory is a second dev container configuration.
It bind-mounts the host's `/dev`, so `idf.py -p /dev/ttyACM0 flash monitor`
works from inside the container. Choose it with **Dev Containers: Rebuild and
Reopen in Container**. The default configuration in `.devcontainer/` has no
device access and is the one Codespaces uses. Both build the same image
(`.devcontainer/Dockerfile`) and share the cache volumes.

## Host setup (Linux)

The container can only open a device that the host user can open. With
rootless Docker the container's `root` is the host user, and the group
`dialout` of the host is not mapped into the container, so being in
`dialout` on the host is not enough. Give the boards' serial devices to the
user with a udev rule, for example `/etc/udev/rules.d/99-trailcamhub.rules`:

```
# Espressif USB-Serial-JTAG (ESP32-C5, ESP32-S3, ESP32-P4)
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"
# WCH CH343/CH340 USB-UART bridges (e.g. on the ESP32-P4-NANO)
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"
```

`MODE="0666"` lets every local user open these devices. On a desktop where
only the logged-in user should have them, use `TAG+="uaccess"` instead of
`MODE="0666"`; this does not work for a user who is only logged in over SSH.

Then reload the rules and plug the boards in again:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
ls -l /dev/serial/by-id/
```

## In the container

```bash
ls -l /dev/serial/by-id/
. "$IDF_PATH/export.sh"
idf.py -B build-c5 -p /dev/ttyACM0 flash monitor
```

The ESP32-C5 and ESP32-S3 dongles enumerate again after a reset into the
bootloader. The bind mount of `/dev` shows the new device node, but its
number can change (`ttyACM0` → `ttyACM1`); the names in `/dev/serial/by-id/`
stay the same.
