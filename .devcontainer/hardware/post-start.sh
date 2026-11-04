#!/usr/bin/env bash
# Runs at every start of the USB boards dev container: /dev belongs to the
# container and starts empty of serial devices. The host's /dev is at /dev/host
# (see devcontainer.json); these links give its serial devices the usual names.
# They may dangle until a board is plugged in.
set -euo pipefail

ln -sfn host/serial /dev/serial
for n in 0 1 2 3 4 5 6 7 8 9; do
    ln -sfn "host/ttyACM$n" "/dev/ttyACM$n"
    ln -sfn "host/ttyUSB$n" "/dev/ttyUSB$n"
done

if ls /dev/serial/by-id/* >/dev/null 2>&1; then
    ls -l /dev/serial/by-id/
fi
