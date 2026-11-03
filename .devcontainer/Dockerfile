# Image of both dev container configurations (.devcontainer/devcontainer.json
# and .devcontainer/hardware/devcontainer.json): the ESP-IDF image of the
# firmware CI, plus the tools that the other CI checks use.
FROM espressif/idf:v5.5.6

# jq for scripts, usbutils (lsusb) to find boards. The base image already has
# build-essential (g++ 13 for the host tests).
RUN apt-get update \
    && apt-get install -y --no-install-recommends jq usbutils \
    && rm -rf /var/lib/apt/lists/*

# clang-format as in CI: Ubuntu 24.04 has no LLVM 23, the PyPI wheel pins it.
RUN python3 -m venv /opt/clang-format \
    && /opt/clang-format/bin/pip install --no-cache-dir clang-format==23.1.3 \
    && ln -s /opt/clang-format/bin/clang-format /usr/local/bin/clang-format

# The image sets up ESP-IDF only in its entrypoint. Interactive shells, and
# through the user environment probe also VS Code and its extensions, get
# idf.py, cmake and the toolchains from here.
RUN printf '\n# ESP-IDF (dev container)\n. "$IDF_PATH/export.sh" >/dev/null 2>&1\n' \
    >> /root/.bashrc
