ARG PROBE_BUILDER_IMAGE=docker-hub.tange365.com/public/ubuntu:22.04
ARG PROBE_RUNTIME_IMAGE=docker-hub.tange365.com/public/ubuntu:22.04
ARG PROBE_INSTALL_PACKAGES=1
FROM ${PROBE_BUILDER_IMAGE} AS builder

ARG TIRTC_STANDARD_SDK_URL
ARG TIRTC_DESKTOP_SDK_URL
ARG PROBE_INSTALL_PACKAGES

ENV DEBIAN_FRONTEND=noninteractive

RUN if [ "$PROBE_INSTALL_PACKAGES" = 1 ]; then \
      apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 update \
      && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 install -y --no-install-recommends build-essential ca-certificates curl make \
      && rm -rf /var/lib/apt/lists/*; \
    else \
      command -v cc && command -v curl && command -v make; \
    fi

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
        all \
    && make PLATFORM=linux-x86_64 \
        BUILD_DIR=build/desktop \
        TIRTC_SDK_DIR=/opt/tirtc-sdk/desktop \
        TIRTC_SDK_VARIANT=tgmp-linux-desktop-standard \
        TIRTC_SDK_SOURCE_URL="$TIRTC_DESKTOP_SDK_URL" \
        all

FROM ${PROBE_RUNTIME_IMAGE}

ARG PROBE_INSTALL_PACKAGES

ENV DEBIAN_FRONTEND=noninteractive

RUN if [ "$PROBE_INSTALL_PACKAGES" = 1 ]; then \
      apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 update \
      && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 install -y --no-install-recommends ca-certificates iproute2 iptables iputils-ping libstdc++6 tcpdump \
      && rm -rf /var/lib/apt/lists/*; \
    else \
      command -v ip && command -v iptables && command -v ping && command -v tcpdump; \
    fi

COPY --from=builder /src/build/standard/linux-x86_64/tirn-probe-device /usr/local/libexec/tirn-probe-device-standard
COPY --from=builder /src/build/desktop/linux-x86_64/tirn-probe-device /usr/local/libexec/tirn-probe-device-desktop
COPY --from=builder /opt/tirtc-sdk/desktop/lib/libwebrtc.so /opt/tirtc/desktop/lib/libwebrtc.so
COPY docker/probe-runner/select-probe.sh /usr/local/bin/tirn-probe-device
COPY docker/probe-runner/entrypoint.sh /usr/local/bin/probe-runner-entrypoint
COPY docker/probe-runner/audio /opt/tirtc-probe-tests/audio
COPY script/run_tirn_probe_with_netem_gateway.sh /opt/tirtc-probe-tests/script/
COPY script/run_tirn_probe_netem_report.sh /opt/tirtc-probe-tests/script/
COPY script/run_tirn_probe_case.py /opt/tirtc-probe-tests/script/
COPY script/generate_tirn_probe_netem_report.py /opt/tirtc-probe-tests/script/
COPY script/run_tirn_probe_idle_1000.sh /opt/tirtc-probe-tests/script/

RUN chmod 0755 \
      /usr/local/bin/tirn-probe-device \
      /usr/local/bin/probe-runner-entrypoint \
      /usr/local/libexec/tirn-probe-device-standard \
      /usr/local/libexec/tirn-probe-device-desktop \
      /opt/tirtc-probe-tests/script/run_tirn_probe_with_netem_gateway.sh \
      /opt/tirtc-probe-tests/script/run_tirn_probe_netem_report.sh \
      /opt/tirtc-probe-tests/script/run_tirn_probe_case.py \
      /opt/tirtc-probe-tests/script/generate_tirn_probe_netem_report.py \
      /opt/tirtc-probe-tests/script/run_tirn_probe_idle_1000.sh

ENV TIRTC_PROBE_TEST_ROOT=/opt/tirtc-probe-tests \
    TIRTC_PROBE_AUDIO_8K=/opt/tirtc-probe-tests/audio/send_audio_8k.opus \
    TIRTC_PROBE_AUDIO_16K=/opt/tirtc-probe-tests/audio/send_audio_16k.opus

WORKDIR /work

ENTRYPOINT ["/usr/local/bin/probe-runner-entrypoint"]
