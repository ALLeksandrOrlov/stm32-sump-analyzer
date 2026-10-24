# STM32 SUMP Analyzer

An 8-channel USB logic analyzer for STM32F407. This quick start takes an
STM32F4 Discovery from a Docker build to a waveform in PulseView.

## Requirements

- **Docker** with Linux containers and Buildx.
- **PulseView 0.4.2**.
- **Hardware:** STM32F4 Discovery, two USB data cables and 3.3 V signals to
  capture. The board has an onboard programmer and a separate USB OTG FS
  connector for the analyzer.

## 1. Install and start Docker

Follow the Docker installation guide for
[macOS](https://docs.docker.com/desktop/setup/install/mac-install/),
[Windows](https://docs.docker.com/desktop/setup/install/windows-install/) or
[Linux](https://docs.docker.com/engine/install/). Start Docker and check:

```sh
docker version
```

The output must include both **Client** and **Server**. On Windows, use Linux
containers. The compiler, build tools and test dependencies run inside Docker.

## 2. Build the firmware

[Download the project ZIP](https://github.com/ALLeksandrOrlov/stm32-sump-analyzer/archive/refs/heads/main.zip),
extract it, and open a terminal in the folder containing `Dockerfile`. Run:

```sh
docker build --target artifacts --output type=local,dest=out/docker .
```

The first build takes several minutes. Docker builds and checks the firmware,
then saves the ready-to-flash files in `out/docker/firmware/`.
No Git, Make, Python or ARM toolchain installation is needed on your computer.

## 3. Flash and connect the board

Using your programmer's utility, flash
`out/docker/firmware/stm32_sump_analyzer.bin` at **`0x08000000`**, then reset
and run the board. The same directory also contains `.hex` and `.elf` files.

Connect the board's **USB OTG FS** port to the computer with a data cable.
On STM32F4 Discovery this is **CN5**, separate from the programmer's CN1.
A USB CDC serial port should appear.

## 4. Select the analyzer in PulseView

Open PulseView 0.4.2 and choose **Connect to Device** from the device selector:

1. Driver: **Openbench Logic Sniffer & SUMP compatibles (ols)**.
2. Interface: **Serial Port**.
3. Port: your board's USB CDC port, such as `COM5` on Windows,
   `/dev/ttyACM0` on Linux or `/dev/cu.usbmodem…` on macOS.
4. Click **Scan for devices using driver above**.
5. Select **STM32 SUMP Analyzer with 8 channels** and click **OK**.

![Selecting the SUMP driver and discovering STM32 SUMP Analyzer in PulseView](docs/images/pulseview-connect.jpg)

The screenshot uses a demo serial port. Select your board's port, not the
`/tmp/stm32-sump-analyzer` path shown in the example or the programmer's serial port.

## 5. Connect a signal and capture a waveform

Channels **0–7** correspond to **PD0–PD7**. Connect the signal source's ground
to the board's **GND** and use **3.3 V logic signals**.

For a first capture, connect an active signal to **PD0**, choose **1 MHz** and
**1 k samples**, leave the trigger disabled, then click **Run**. Keep the sample
count at or below **28,672** and leave RLE, DDR and external clock disabled.
Zoom into the trace with the mouse wheel to inspect its edges.

A disconnected or constant input produces a flat trace. For an intermittent
signal, enable a rising or falling edge trigger on one channel, click **Run**,
and then generate the signal. The capture waits until that edge arrives.

## 6. Decode a bus: I²C example

To capture an active 3.3 V I²C bus, connect:

| Bus signal | Analyzer pin | PulseView channel |
| --- | --- | --- |
| SCL | PD0 | 0 |
| SDA | PD1 | 1 |
| GND | GND | — |

For a 100 kHz bus, start with **1 MHz** sampling and **1 k samples**. For an
intermittent bus, select a falling edge trigger on channel 1, click **Run**,
then generate a transaction. Press **D**, find **I²C** and double-click it to add the decoder.
In its settings, assign **SCL → 0** and **SDA → 1**. Zoom in to see the clock,
data, address, data bytes and acknowledgements together.

![I2C waveforms and decoded address, data bytes and acknowledgements in PulseView](docs/images/pulseview-i2c.jpg)

This example uses a synthetic transaction to address `0x50`, writing `0x10`
and `0x2A`. Your decoded values come from the connected bus.

[Developer documentation](docs/README.md)
