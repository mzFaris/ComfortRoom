#!/usr/bin/env bash
# Develop, build, serve and simulate the dashboard with Podman.
#
#   scripts/dashboard.sh dev        # Vite dev server on http://localhost:8080
#   scripts/dashboard.sh build      # build localhost/comfortroom-dashboard:latest
#   scripts/dashboard.sh serve      # serve the built image on port 8080
#   scripts/dashboard.sh simulate   # publish synthetic telemetry to the broker
#   scripts/dashboard.sh shell      # shell with node_modules installed
#   scripts/dashboard.sh stop       # remove dev/serve containers
#
# Overrides: DASHBOARD_IMAGE, NODE_IMAGE, MQTT_URL, DEVICE_ID, INTERVAL_MS.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DASHBOARD="$ROOT/dashboard"
IMAGE="${DASHBOARD_IMAGE:-localhost/comfortroom-dashboard:latest}"
NODE_IMAGE="${NODE_IMAGE:-docker.io/library/node:22-alpine}"

usage() {
  sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# Only install when node_modules is missing: `npm ci` is deterministic but
# deletes the existing tree, which makes every restart slow.
INSTALL_DEPS='if [ ! -d node_modules ]; then npm ci; fi'

case "${1:-dev}" in
  dev)
    podman run --rm -it \
      --name comfortroom-dashboard-dev --replace \
      -v "$DASHBOARD:/app:Z" -w /app \
      -p 8080:8080 \
      "$NODE_IMAGE" \
      sh -lc "$INSTALL_DEPS; exec npm run dev -- --host 0.0.0.0"
    ;;
  build)
    podman build -t "$IMAGE" \
      -f "$ROOT/containers/dashboard/Containerfile" \
      "$ROOT"
    ;;
  serve)
    podman run -d \
      --name comfortroom-dashboard --replace \
      -p 8080:8080 \
      "$IMAGE"
    echo "dashboard: http://localhost:8080"
    ;;
  simulate)
    podman run --rm -it --network host \
      -v "$DASHBOARD:/app:Z" -w /app \
      -e MQTT_URL -e DEVICE_ID -e INTERVAL_MS -e MOTION_HOLD_MS \
      "$NODE_IMAGE" \
      sh -lc "$INSTALL_DEPS; exec npm run simulate"
    ;;
  shell)
    podman run --rm -it \
      -v "$DASHBOARD:/app:Z" -w /app \
      "$NODE_IMAGE" \
      sh -lc "$INSTALL_DEPS; exec sh"
    ;;
  stop)
    podman rm -f comfortroom-dashboard comfortroom-dashboard-dev 2>/dev/null || true
    ;;
  *)
    usage
    exit 1
    ;;
esac
