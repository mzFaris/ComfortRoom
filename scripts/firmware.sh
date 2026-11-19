#!/usr/bin/env bash
# Compile, flash and monitor the ESP32 firmware inside a Podman container.
#
#   scripts/firmware.sh compile
#   scripts/firmware.sh upload      # TTY_DEV=/dev/ttyUSB0 by default
#   scripts/firmware.sh monitor
#   scripts/firmware.sh shell
#
# Overrides: FIRMWARE_IMAGE, FQBN, TTY_DEV, BAUD.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${FIRMWARE_IMAGE:-localhost/comfortroom-firmware:latest}"
FQBN="${FQBN:-esp32:esp32:esp32}"
TTY_DEV="${TTY_DEV:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"
SKETCH="firmware/ComfortRoom"

usage() {
  sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

run_tool() {
  local -a podman_args=()
  if [[ "${1:-}" == "--device" ]]; then
    podman_args=(--device "$TTY_DEV")
    shift
  fi
  podman run --rm -it \
    "${podman_args[@]}" \
    -v "$ROOT:/workspace:Z" \
    -v comfortroom-firmware-cache:/root/.cache \
    -e XDG_CACHE_HOME=/root/.cache \
    -w /workspace \
    "$IMAGE" \
    "$@"
}

case "${1:-compile}" in
  compile)
    ensure_image
    run_tool arduino-cli compile --fqbn "$FQBN" "$SKETCH"
    ;;
  upload)
    require_tty
    ensure_image
    # On rootless Podman, if the device is not writable add
    # --group-add keep-groups to the run_tool call below.
    run_tool --device "$TTY_DEV" \
      arduino-cli upload -p "$TTY_DEV" --fqbn "$FQBN" "$SKETCH"
    ;;
  monitor)
    require_tty
    ensure_image
    run_tool --device "$TTY_DEV" \
      arduino-cli monitor -p "$TTY_DEV" --config "baudrate=$BAUD"
    ;;
  shell)
    ensure_image
    run_tool bash
    ;;
  image)
    build_image
    ;;
  *)
    usage
    exit 1
    ;;
esac
