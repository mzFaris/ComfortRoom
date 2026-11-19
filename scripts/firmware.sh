#!/usr/bin/env bash
# Compile, flash and monitor the ESP32 firmware inside a Podman container.
#
# The ESP32 core lives in the comfortroom-arduino-core Podman volume and is
# installed on first use; the image only carries arduino-cli and the libraries.
# Override the core version with CORE_VERSION (default 4.0.0).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${FIRMWARE_IMAGE:-localhost/comfortroom-firmware:latest}"
FQBN="${FQBN:-esp32:esp32:esp32}"
TTY_DEV="${TTY_DEV:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"
CORE_VERSION="${CORE_VERSION:-4.0.0}"
CORE_SPEC="esp32:esp32@${CORE_VERSION}"
CORE_VOLUME="${FIRMWARE_CORE_VOLUME:-comfortroom-arduino-core}"
SKETCH="firmware/ComfortRoom"

usage() {
  cat <<'EOF'
Usage: scripts/firmware.sh <command>

  compile       Compile firmware/ComfortRoom (installs the ESP32 core on first use)
  upload        Upload to the board (TTY_DEV, default /dev/ttyUSB0)
  monitor       Serial monitor (BAUD, default 115200)
  shell         Shell inside the toolchain image
  core-status   Show the ESP32 core installed in the volume
  core-reset    Delete the core volume; the next compile reinstalls it
  image         Build the toolchain image

Environment:
  FQBN            Board FQBN (default esp32:esp32:esp32)
  TTY_DEV         Serial port for upload/monitor (default /dev/ttyUSB0)
  BAUD            Monitor baud rate (default 115200)
  CORE_VERSION    ESP32 core version to install (default 4.0.0)
  FIRMWARE_CORE_VOLUME  Podman volume name (default comfortroom-arduino-core)
  FIRMWARE_IMAGE  Toolchain image (default localhost/comfortroom-firmware:latest)
EOF
}

build_image() {
  podman build -t "$IMAGE" \
    -f "$ROOT/containers/firmware/Containerfile" \
    "$ROOT/containers/firmware"
}

ensure_image() {
  podman image exists "$IMAGE" || build_image
}

require_tty() {
  if [[ ! -e "$TTY_DEV" ]]; then
    echo "error: $TTY_DEV not found; set TTY_DEV to the board's serial port" >&2
    exit 1
  fi
}

# Usage: run_tool [-e VAR=VALUE] [--device] <command...>
run_tool() {
  local -a podman_args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -e)
        podman_args+=(-e "$2")
        shift 2
        ;;
      --device)
        podman_args+=(--device "$TTY_DEV")
        shift
        ;;
      *)
        break
        ;;
    esac
  done
  # :z keeps the shared volumes usable across containers; :Z would relabel
  # them to one container's MCS categories and the next run would be denied.
  podman run --rm -it \
    "${podman_args[@]}" \
    -v "$ROOT:/workspace:Z" \
    -v "$CORE_VOLUME:/arduino/data/packages:z" \
    -v comfortroom-firmware-cache:/root/.cache:z \
    -e XDG_CACHE_HOME=/root/.cache \
    -w /workspace \
    "$IMAGE" \
    "$@"
}

ensure_core() {
  run_tool -e CORE_SPEC="$CORE_SPEC" -e CORE_VERSION="$CORE_VERSION" sh -lc '
    set -eu
    marker=/arduino/data/packages/.comfortroom-core

    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$CORE_SPEC" ]; then
      exit 0
    fi

    # Serialise concurrent first runs against the shared volume.
    exec 9>/arduino/data/packages/.comfortroom-core.lock
    flock 9

    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$CORE_SPEC" ]; then
      exit 0
    fi

    echo "Installing ESP32 core $CORE_SPEC (first run downloads ~1.5 GB)..." >&2
    arduino-cli core update-index
    installed="$(arduino-cli core list --no-color 2>/dev/null | awk '\''$1 == "esp32:esp32" { print $2; exit }'\'')"
    if [ -n "$installed" ] && [ "$installed" != "$CORE_VERSION" ]; then
      echo "Replacing ESP32 core $installed with $CORE_VERSION..." >&2
      arduino-cli core uninstall esp32:esp32 || true
    fi
    arduino-cli core install "$CORE_SPEC"
    printf "%s" "$CORE_SPEC" > "$marker"
  '
}

case "${1:-compile}" in
  compile)
    ensure_image
    ensure_core
    run_tool arduino-cli compile --fqbn "$FQBN" "$SKETCH"
    ;;
  upload)
    require_tty
    ensure_image
    ensure_core
    # On rootless Podman, if the device is not writable add
    # --group-add keep-groups to the run_tool call below.
    run_tool --device \
      arduino-cli upload -p "$TTY_DEV" --fqbn "$FQBN" "$SKETCH"
    ;;
  monitor)
    require_tty
    ensure_image
    run_tool --device \
      arduino-cli monitor -p "$TTY_DEV" --config "baudrate=$BAUD"
    ;;
  shell)
    ensure_image
    run_tool bash
    ;;
  core-status)
    ensure_image
    run_tool arduino-cli core list
    ;;
  core-reset)
    podman volume rm -f "$CORE_VOLUME" >/dev/null 2>&1 || true
    echo "removed volume $CORE_VOLUME; the next compile reinstalls the core"
    ;;
  image)
    build_image
    ;;
  *)
    usage
    exit 1
    ;;
esac
