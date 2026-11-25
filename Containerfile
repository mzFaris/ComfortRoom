FROM docker.io/library/eclipse-mosquitto:2

COPY mosquitto.conf /mosquitto/config/mosquitto.conf

RUN mkdir -p /mosquitto/data /mosquitto/log

EXPOSE 1883

VOLUME ["/mosquitto/data"]

# Podman's default OCI image format drops HEALTHCHECK; build with
# `--format docker` if you want it honored.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD mosquitto_sub -h 127.0.0.1 -p 1883 -t '$SYS/broker/version' -C 1 -W 3 >/dev/null || exit 1

# The base image's entrypoint chowns /mosquitto when started as root;
# mosquitto then drops to the `mosquitto` user itself. Keep it that way so a
# freshly created data volume is writable.
