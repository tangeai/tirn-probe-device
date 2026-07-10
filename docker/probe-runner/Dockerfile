FROM docker-hub.tange365.com/public/ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 update \
    && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=20 install -y --no-install-recommends ca-certificates iproute2 iptables iputils-ping tcpdump \
    && rm -rf /var/lib/apt/lists/*

COPY build/linux-x86_64/tirtc_accel_device_probe /usr/local/bin/tirtc_accel_device_probe

RUN chmod 0755 /usr/local/bin/tirtc_accel_device_probe

WORKDIR /work
