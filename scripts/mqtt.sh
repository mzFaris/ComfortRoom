#!/usr/bin/env bash
# Run the local MQTT broker in Podman: MQTT 1883 + WebSockets 9001.
#
#   scripts/mqtt.sh up
#   scripts/mqtt.sh down
#   scripts/mqtt.sh logs
#   scripts/mqtt.sh shell
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${MOSQUITTO_IMAGE:-docker.io/library/eclipse-mosquitto:2}"
NAME=comfortroom-mqtt
CONF="$ROOT/containers/mosquitto/mosquitto.conf"

case "${1:-up}" in
  up)
    podman run -d \
      --name "$NAME" --replace \
      -p 1883:1883 \
      -p 9001:9001 \
      -v "$CONF:/mosquitto/config/mosquitto.conf:ro,Z" \
      "$IMAGE" \
      mosquitto -c /mosquitto/config/mosquitto.conf
    echo "mqtt: localhost:1883 (tcp), ws://localhost:9001/mqtt (websockets)"
    ;;
  down)
    podman rm -f "$NAME" 2>/dev/null || true
    ;;
  logs)
    podman logs -f "$NAME"
    ;;
  shell)
    podman exec -it "$NAME" sh
    ;;
  *)
    sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
