# FreeRTOS Kernel

This directory contains the unmodified subset of the official FreeRTOS Kernel
used by the STM32F407 firmware.

- Source: [FreeRTOS/FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel)
- Release: [V11.3.1](https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.3.1)
- Commit: `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d`
- Annotated tag object: `054e14f3397023aa83813a65aa065fc4597d481b`
- License: [MIT](LICENSE.md)
- Port: `portable/GCC/ARM_CM4F`

The selected sources provide tasks, queues, lists, stream buffers and message
buffers. Required public headers are included. Heap implementations, software
timer implementations, other ports, examples and upstream build systems are not
part of the firmware dependency.

`SHA256SUMS` records every upstream file in this directory. Check it from here:

```sh
sha256sum --check SHA256SUMS
```

On macOS use `shasum -a 256 --check SHA256SUMS`. Verify the source commit and
review upstream release notes before replacing the dependency. Keep upstream
files unchanged, preserve their copyright notices and regenerate the manifest
from the selected files. Project configuration and hooks live in
`platform/freertos/`; the kernel and its Cortex-M4F port use the same files in
firmware and QEMU scheduler tests.
