FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251 AS toolchain
RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc libc6-dev gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi \
    libnewlib-dev python3 qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*

FROM toolchain AS qemu-builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ git ca-certificates python3-venv ninja-build pkg-config bzip2 \
    libglib2.0-dev libfdt-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/qemu-venv && /opt/qemu-venv/bin/pip install meson==1.5.0
ENV PATH="/opt/qemu-venv/bin:${PATH}"
RUN git clone --depth 1 --branch v9.2.4 https://github.com/qemu/qemu.git /opt/qemu-src \
    && test "$(git -C /opt/qemu-src rev-parse HEAD)" = b2995afed254e4804611df52210067a4d5dbdf0b
COPY qemu /opt/analyzer-qemu
RUN python3 /opt/analyzer-qemu/apply_overlay.py /opt/qemu-src
WORKDIR /opt/qemu-build
RUN /opt/qemu-src/configure --target-list=arm-softmmu --disable-docs \
    --disable-tools --disable-user --disable-plugins --disable-modules \
    --disable-slirp --disable-gtk --disable-sdl --disable-vnc --disable-curses \
    --disable-opengl --disable-virglrenderer --disable-pixman --disable-rust \
    --audio-drv-list= \
    && ninja -j2 qemu-system-arm

# Reusable CI image: project sources are mounted at runtime, so test runs never
# disappear behind a Docker RUN cache hit. QEMU is cached independently.
FROM toolchain AS ci
RUN apt-get update && apt-get install -y --no-install-recommends gcovr \
    && rm -rf /var/lib/apt/lists/*
COPY --from=qemu-builder /opt/qemu-build/qemu-system-arm /usr/local/bin/qemu-system-arm-sump-analyzer
WORKDIR /project
CMD ["python3", "scripts/ci.py"]

# Optional standalone BuildKit export, with the same complete pipeline.
FROM ci AS build
COPY . .
ARG SOURCE_REVISION=unknown
ARG SOURCE_REF=local
ARG SOURCE_DIRTY=unknown
RUN SOURCE_REVISION="$SOURCE_REVISION" SOURCE_REF="$SOURCE_REF" SOURCE_DIRTY="$SOURCE_DIRTY" \
    python3 scripts/ci.py

FROM scratch AS artifacts
COPY --from=build /project/out/ci/dist/ /firmware/
COPY --from=build /project/out/ci/reports/ /reports/
