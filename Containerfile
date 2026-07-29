# Build linux/amd64 xrd-readgen against AlmaLinux 10 (el10) + XRootD client.
# Usage: see scripts/build-release.sh
ARG ALMA_VERSION=10
FROM --platform=linux/amd64 almalinux:${ALMA_VERSION}

RUN dnf -y install epel-release \
 && dnf -y install 'dnf-command(config-manager)' \
 && dnf config-manager --set-enabled crb \
 && dnf -y install \
      cmake \
      gcc-c++ \
      make \
      git \
      openssl-devel \
      libcurl-devel \
      xrootd-client-devel \
 && dnf clean all

WORKDIR /src
COPY . /src

RUN cmake -S . -B /build \
      -DCMAKE_BUILD_TYPE=Release \
      -DREADGEN_ENABLE_TESTS=OFF \
 && cmake --build /build -j"$(nproc)" --target xrd-readgen \
 && mkdir -p /out \
 && cp /build/xrd-readgen /out/xrd-readgen \
 && /out/xrd-readgen version

CMD ["true"]
