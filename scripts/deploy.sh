#!/usr/bin/env bash
# Install/remove ComfortRoom as Podman Quadlet units under a systemd user
# session, so the broker and dashboard start on login and restart on failure.
#
#   scripts/deploy.sh install     # build the dashboard image + enable services
#   scripts/deploy.sh uninstall   # disable and remove the units
#   scripts/deploy.sh status
#   scripts/deploy.sh logs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="${QUADLET_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd}"
UNITS=(comfortroom-mqtt.service comfortroom-dashboard.service)

usage() {
  sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

case "${1:-install}" in
  install)
    "$ROOT/scripts/dashboard.sh" build
    mkdir -p "$UNIT_DIR"
    for template in "$ROOT"/deploy/*.container.in; do
      unit="$(basename "${template%.in}")"
      sed "s|@REPO@|$ROOT|g" "$template" >"$UNIT_DIR/$unit"
      echo "wrote $UNIT_DIR/$unit"
    done
    systemctl --user daemon-reload
    systemctl --user enable --now "${UNITS[@]}"
    echo "dashboard: http://localhost:8080"
    ;;
  uninstall)
    systemctl --user disable --now "${UNITS[@]}" 2>/dev/null || true
    rm -f "$UNIT_DIR/comfortroom-mqtt.container" \
          "$UNIT_DIR/comfortroom-dashboard.container"
    systemctl --user daemon-reload
    ;;
  status)
    systemctl --user status --no-pager "${UNITS[@]}" || true
    ;;
  logs)
    journalctl --user -u "${UNITS[@]}" -f
    ;;
  *)
    usage
    exit 1
    ;;
esac
