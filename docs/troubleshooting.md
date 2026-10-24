# Troubleshooting

## Docker is unavailable

```sh
docker version
docker context ls
```

`docker version` must show both **Client** and **Server**. Start Docker Engine
on Linux, or Docker Desktop or your Linux VM on macOS. A
`Cannot connect to the Docker daemon` error occurs before the build and is
unrelated to the firmware.

Check that Buildx is available:

```sh
docker buildx version
```

Linux needs Docker Engine and the Buildx plugin. On macOS, Docker Desktop or
Docker Engine inside Lima can provide the Linux environment. The VM must have
write access to the repository directory. For the QEMU build, allow 4 CPUs,
4 GiB of memory and approximately 20 GiB of disk space for images and caches.

If your Lima VM and Docker context are named `sump-docker`, use:

```sh
limactl start sump-docker
docker context use sump-docker
limactl stop sump-docker
```

The last command stops the VM while preserving its images. Substitute your own
VM name as needed; the project does not require a particular VM name or Docker
installation method. After moving the repository, check Docker Desktop's shared
directories or Lima's mounts.

## The Docker image fails to build

```sh
docker buildx build --progress=plain --target ci --load -t stm32-sump-analyzer-ci:local .
```

Find the first error, rather than only the final `failed to solve` message.
Check access to Docker Hub, Debian, PyPI, GitHub and GitLab: image preparation
downloads the base image, packages and pinned QEMU dependencies. Update the
Dockerfile when changing the toolchain, then run the complete pipeline again.
Release artifacts require all checks to pass.

## A test stage fails

Read `out/ci/reports/summary.md`, then the log with the same name as the failing
stage. In GitHub Actions, download the `test-reports-<commit>` artifact. Checks
continue after the first failure, so a run can expose several independent issues.

- `pipeline`: Python unit tests for CI safeguards, dependency integrity and
  test tooling. The traceback identifies the failed contract.
- `cubemx`: changes outside `USER CODE` sections or unmatched section markers.
  After deliberate regeneration, review the diff before updating the baseline.
- `native` / `sanitizers`: failed assertions, memory errors, undefined behavior
  or violations of the USB transport contract.
- `coverage`: GCC/gcov compilation, native test execution, report generation or
  the 95% line / 90% branch thresholds. Find the untested behavior in the HTML
  report before adding a test; lowering the threshold does not address the gap.
- `qemu-core`: test ELF startup, semihosting or portable core behavior.
- `qemu-stm32`: peripheral configuration, DMA, EXTI, IRQ handling or the analyzer model.
- `qemu-rtos`: IRQ priorities, scheduler startup, message buffers, task
  notifications, TX buffer ownership or USB session recovery.
- `firmware`: compiler or linker errors, memory limits or STM32/FreeRTOS/USB integration.

`unsupported machine type stm32f407-sump-analyzer` means the command selected
stock QEMU. Use the project container and `QEMU=qemu-system-arm-sump-analyzer`
for peripheral and RTOS tests.

`QEMU failed: ... timed out` means the test stalled or took too long. Check
whether execution reaches `Reset_Handler` and whether the CPU entered a fault
handler. See the [QEMU guide](qemu.md) for the model's scope.

## A FreeRTOS assertion fires on the board

Inspect `failure_file` and `failure_line` from `freertos_support.c` in a debugger.
The USB IRQ must have priority 6 and be enabled by the TX task's `activate()`
call after scheduler startup. Capture IRQs at priority 1 cannot call FreeRTOS
APIs; they notify tasks through the TIM6_DAC interrupt at priority 5.
Message buffers reserve notification index 0; application events use index 1.

If USB hardware initialization stalls in `HAL_Delay`, inspect SysTick and the
restoration of BASEPRI after static task creation. Before scheduler startup,
the tick must serve HAL; afterward, it must also serve the FreeRTOS port. The
[CubeMX guide](cubemx.md) describes how these handlers are connected.

## Output files are missing or unwritable on macOS

The Docker daemon inside the VM must see the project's bind mount. Allow access
to the directory in Docker Desktop, or check the Lima mounts. The pipeline runs
the container with the current user's UID/GID so generated files remain owned
by that user. Fix the directory permissions instead of using
`sudo make docker-build`.

## A release was not published

1. Confirm that a tag such as `v0.1.0` was pushed, rather than only a commit or branch.
2. Check that **Docker / native / QEMU / firmware** passed completely.
3. Inspect the release job log for checksum, source commit, tag format or
   `contents: write` permission errors.
4. Check whether a release already exists for the tag. The pipeline does not replace it.

Manual `workflow_dispatch` runs and pull requests build artifacts without
publishing a release. GitHub Actions builds the pushed commit; uncommitted
local changes are not included.
