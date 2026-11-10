# syntax=docker/dockerfile:1

# Versions are pinned to the combination verified on macOS. Ubuntu's protobuf
# packages are too old (3.21) for the json/ API used by schema.cpp, so both
# Abseil and Protobuf are built from source here.
ARG UBUNTU_VERSION=24.04
ARG ABSEIL_VERSION=20250512.1
ARG PROTOBUF_VERSION=36.2

FROM ubuntu:${UBUNTU_VERSION} AS deps

ARG ABSEIL_VERSION
ARG PROTOBUF_VERSION

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/src

RUN curl -fsSLO "https://github.com/abseil/abseil-cpp/archive/refs/tags/${ABSEIL_VERSION}.tar.gz" \
 && tar xf "${ABSEIL_VERSION}.tar.gz" \
 && cmake -S "abseil-cpp-${ABSEIL_VERSION}" -B absl-build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_CXX_STANDARD_REQUIRED=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DABSL_PROPAGATE_CXX_STD=ON \
      -DABSL_ENABLE_INSTALL=ON \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_INSTALL_PREFIX=/opt/prp \
 && cmake --build absl-build -j"$(nproc)" \
 && cmake --install absl-build

RUN curl -fsSLO "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v${PROTOBUF_VERSION}.tar.gz" \
 && tar xf "v${PROTOBUF_VERSION}.tar.gz" \
 && cmake -S "protobuf-${PROTOBUF_VERSION}" -B protobuf-build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_CXX_STANDARD_REQUIRED=ON \
      -Dprotobuf_BUILD_SHARED_LIBS=ON \
      -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_BUILD_LIBPROTOC=ON \
      -Dprotobuf_WITH_ZLIB=OFF \
      -Dprotobuf_LOCAL_DEPENDENCIES_ONLY=ON \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_PREFIX_PATH=/opt/prp \
      -DCMAKE_INSTALL_PREFIX=/opt/prp \
 && cmake --build protobuf-build -j"$(nproc)" \
 && cmake --install protobuf-build

FROM deps AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libboost-dev \
      python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src/app
COPY . .

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/prp \
      -DPRP_BUILD_TESTS=ON \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      libatomic1 \
      libgcc-s1 \
      libstdc++6 \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /opt/prp/lib /data/recordings

COPY --from=deps /opt/prp/lib/*.so* /opt/prp/lib/
RUN printf '/opt/prp/lib\n' > /etc/ld.so.conf.d/prp.conf \
 && ldconfig

COPY --from=build /src/app/build/protorecprep /usr/local/bin/protorecprep

WORKDIR /data
EXPOSE 5000

ENTRYPOINT ["protorecprep"]
CMD ["record", "--config", "etc/docker.ini"]
