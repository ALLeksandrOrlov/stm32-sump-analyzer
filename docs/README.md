# Developer documentation

The [project README](../README.md) covers the user workflow from installing
Docker to capturing and decoding a waveform in PulseView.

## Development

- [Contributing](contributing.md): module ownership, readable code and change review.
- [Building](building.md): optional native builds and individual test targets.
- [Architecture](architecture.md): dependencies, ECA, FreeRTOS tasks, interrupts and buffer ownership.
- [Device reference](device-reference.md): input mapping, protocol behavior and supported capture modes.
- [Naming](naming.md): device identity, artifacts, module names and units.
- [CubeMX](cubemx.md): integration points and regeneration boundaries.
- [FreeRTOS Kernel](../third_party/FreeRTOS-Kernel/README.md): pinned sources, license and checksums.

## Verification and releases

- [CI/CD](ci-cd.md): Docker pipeline, test stages, reports, caching and artifacts.
- [Verification](verification.md): test contracts, coverage and test selection.
- [QEMU](qemu.md): peripheral model, build instructions and emulation limits.
- [Hardware validation](hardware-validation.md): USB, signal capture, triggers and timing.
- [Hardware smoke test](hardware-smoke.md): repeatable SUMP checks through a serial port.
- [PulseView screenshots](pulseview-screenshots.md): reproduce the screenshots with a synthetic SUMP device.
- [Releases](releases.md): versions, tags and firmware publication.
- [Troubleshooting](troubleshooting.md): Docker, test, USB and RTOS diagnostics.
