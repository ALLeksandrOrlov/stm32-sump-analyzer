# PulseView screenshots and test transport

The screenshots in `docs/images/pulseview-connect.jpg` and
`docs/images/pulseview-i2c.jpg` were captured in PulseView 0.4.2 from Debian
Bookworm, using libsigrok 0.5.2. The device connects through a virtual serial
port; its name comes from the SUMP metadata in `core/sump_protocol.c`.
The fixture also uses `analyzer_app` and `capture`, replacing the transport
and capture driver.

## Reproduce the screenshots

Run from the repository root:

```sh
docker build -t stm32-sump-analyzer-pulseview:local -f tools/pulseview/Dockerfile .
docker run --rm --name stm32-sump-pulseview \
  -p 127.0.0.1:6080:6080 \
  --mount "type=bind,src=$PWD,dst=/project" \
  stm32-sump-analyzer-pulseview:local
```

Open <http://localhost:6080/vnc.html?autoconnect=true&resize=scale>.
The container starts PulseView with the `ols` driver and the serial port
`/tmp/stm32-sump-analyzer`. To discover the device manually, choose
**Openbench Logic Sniffer & SUMP compatibles**, **Serial Port** and that path.
Scanning should find **STM32 SUMP Analyzer** with eight channels.

For the I²C example, select **1 MHz**, **1 k samples**, channel 0 for SCL and
channel 1 for SDA. Click **Run**, then press **D**, find **I²C** and double-click
to add the decoder. Set **SCL → 0** and **SDA → 1** in the decoder settings.
Rename the channels SCL and SDA if desired; use **+** and **Home** to inspect
the first transaction.

The fixture repeats a write to address `0x50` with bytes `0x10`, `0x2A` and
ACKs every 400 samples. The other six channels remain low. At a selected
sample rate of 1 MHz, the synthetic SCL corresponds to 100 kHz. The fixture
does not model elapsed physical time or change its pattern with the selected
sample rate.

Logs and the fixture executable are written to `out/pulseview/`. Stop the
session with:

```sh
docker stop stm32-sump-pulseview
```

## What the fixture verifies

The synthetic signal exercises device discovery, metadata, sample delivery
and decoding. It does not verify physical USB, STM32 GPIO/DMA or the board's
electrical characteristics. The README captions identify the demonstration
port and synthetic transaction so they are not mistaken for hardware captures.

The container builds libserialport 0.1.1 from pinned commit
`348a6d353af8ac142f68fbf9fe0f4d070448d945`. Its PTY adaptation disables hardware
metadata discovery and treats unsupported modem-control ioctls as inactive
lines only for `/dev/pts/*`. It does not replace protocol bytes. This adaptation
is confined to the screenshot environment; it is not part of the firmware or
main CI image.

The OLS driver enables the FPGA filter flag automatically at ordinary sample
rates. The firmware accepts that hint without modifying raw GPIO samples.
To check decoding without the GUI, first stop acquisition in PulseView, then run:

```sh
docker exec stm32-sump-pulseview sigrok-cli \
  --driver ols:conn=/tmp/stm32-sump-analyzer --config samplerate=1m \
  --samples 1000 -P i2c:scl=0:sda=1
```

The output should include `Address write: 50`, `Data write: 10`,
`Data write: 2A`, ACK and Stop. For interface controls, see the
[PulseView 0.4.2 manual](https://sigrok.org/doc/pulseview/0.4.2/manual.html).

VNC listens on loopback inside the container. The web interface is published
only on the host's `127.0.0.1`; this fixture is intended for local development.
