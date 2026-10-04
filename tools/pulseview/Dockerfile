FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251
RUN apt-get update && apt-get install -y --no-install-recommends \
    pulseview sigrok-cli xvfb x11vnc novnc websockify openbox gcc libc6-dev python3 \
    git ca-certificates autoconf automake libtool make \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --branch libserialport-0.1.1 --depth 1 \
    https://github.com/sigrokproject/libserialport.git /opt/libserialport \
    && test "$(git -C /opt/libserialport rev-parse HEAD)" = 348a6d353af8ac142f68fbf9fe0f4d070448d945
COPY tools/pulseview/enable_pty.py /opt/enable_pty.py
RUN python3 /opt/enable_pty.py /opt/libserialport/serialport.c \
    && cd /opt/libserialport && ./autogen.sh \
    && CPPFLAGS=-DNO_PORT_METADATA ./configure --prefix=/opt/pty-serialport \
    && make -j2 && make install
ENV LD_LIBRARY_PATH=/opt/pty-serialport/lib
WORKDIR /project
ENV DISPLAY=:99
CMD ["sh", "tools/pulseview/start.sh"]
