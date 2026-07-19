FROM docker-hub.tange365.com/public/ubuntu:22.04 AS builder

ARG TIRTC_STANDARD_SDK_URL
ARG TIRTC_DESKTOP_SDK_URL

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 update \
    && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 install -y --no-install-recommends build-essential ca-certificates curl make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile /src/Makefile
COPY src /src/src

RUN test -n "$TIRTC_STANDARD_SDK_URL" \
    && test -n "$TIRTC_DESKTOP_SDK_URL" \
    && mkdir -p /opt/tirtc-sdk/standard /opt/tirtc-sdk/desktop \
    && curl -fL --retry 3 "$TIRTC_STANDARD_SDK_URL" -o /tmp/tirtc-standard.tgz \
    && curl -fL --retry 3 "$TIRTC_DESKTOP_SDK_URL" -o /tmp/tirtc-desktop.tgz \
    && tar -xzf /tmp/tirtc-standard.tgz -C /opt/tirtc-sdk/standard --strip-components=1 \
    && tar -xzf /tmp/tirtc-desktop.tgz -C /opt/tirtc-sdk/desktop --strip-components=1 \
    && test -f /opt/tirtc-sdk/standard/include/tirtc/tiRTC.h \
    && test -f /opt/tirtc-sdk/standard/lib/libTiRTC.a \
    && test -f /opt/tirtc-sdk/desktop/include/tirtc/tiRTC.h \
    && test -f /opt/tirtc-sdk/desktop/lib/libTiRTC.a \
    && test -f /opt/tirtc-sdk/desktop/lib/libwebrtc.so

RUN make PLATFORM=linux-x86_64 \
        BUILD_DIR=build/standard \
        TIRTC_SDK_DIR=/opt/tirtc-sdk/standard \
        TIRTC_SDK_VARIANT=tgmp-linux-standard \
        TIRTC_SDK_SOURCE_URL="$TIRTC_STANDARD_SDK_URL" \
        probe \
    && make PLATFORM=linux-x86_64 \
        BUILD_DIR=build/desktop \
        TIRTC_SDK_DIR=/opt/tirtc-sdk/desktop \
        TIRTC_SDK_VARIANT=tgmp-linux-desktop-standard \
        TIRTC_SDK_SOURCE_URL="$TIRTC_DESKTOP_SDK_URL" \
        probe

FROM docker-hub.tange365.com/public/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 update \
    && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 install -y --no-install-recommends ca-certificates iproute2 iptables iputils-ping libstdc++6 tcpdump \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/standard/linux-x86_64/tirtc_accel_device_probe /usr/local/libexec/tirtc_accel_device_probe-standard
COPY --from=builder /src/build/desktop/linux-x86_64/tirtc_accel_device_probe /usr/local/libexec/tirtc_accel_device_probe-desktop
COPY --from=builder /opt/tirtc-sdk/desktop/lib/libwebrtc.so /opt/tirtc/desktop/lib/libwebrtc.so
COPY docker/probe-runner/select-probe.sh /usr/local/bin/tirtc_accel_device_probe
COPY docker/probe-runner/entrypoint.sh /usr/local/bin/probe-runner-entrypoint

RUN chmod 0755 \
      /usr/local/bin/tirtc_accel_device_probe \
      /usr/local/bin/probe-runner-entrypoint \
      /usr/local/libexec/tirtc_accel_device_probe-standard \
      /usr/local/libexec/tirtc_accel_device_probe-desktop

WORKDIR /work

ENTRYPOINT ["/usr/local/bin/probe-runner-entrypoint"]
