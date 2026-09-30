# Amazon's 4.9 tree needs an old compiler; Debian buster ships GCC 8.3.
FROM debian:buster
RUN sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g; /buster-updates/d' /etc/apt/sources.list \
 && echo 'Acquire::Check-Valid-Until false;' > /etc/apt/apt.conf.d/99no-check-valid \
 && apt-get -qq update \
 && apt-get -qq install -y --no-install-recommends \
      make bc bison flex libssl-dev libelf-dev kmod python \
      gcc gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libc6-dev \
 && rm -rf /var/lib/apt/lists/*
ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
