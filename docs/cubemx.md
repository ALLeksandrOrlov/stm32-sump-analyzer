# CubeMX regeneration

`stm32_sump_analyzer.ioc` sets `ProjectManager.KeepUserCode=true`. Manual changes
to generated files in `Src/` and `Inc/` must stay between the appropriate
`USER CODE BEGIN/END` markers. This applies to comments, headers, definitions and
interrupt handler code.

- `main.c`: includes, `analyzer_board_init()` in SysInit, and the scheduler start
  through `analyzer_board_start()` in section 2. A task processes commands; after
  a successful scheduler start, the generated main loop does not run.
- `stm32f4xx_it.c`: includes, integration of the FreeRTOS SVC/PendSV handlers,
  SysTick additions, and EXTI/TIM8/DMA/TIM6_DAC adapters in section 1.
- `usbd_cdc_if.c`: includes and callbacks 3/4/6/7. USB callbacks copy data and
  notify tasks. They do not parse commands, control capture or wait for TX.
- `usb_device.c`: Includes substitutes the class table from
  `usb_cdc_port_class()`; USB_DEVICE_Init_PostTreatment removes the override.
  This lets the adapter observe IN completion while retaining the other ST
  library callbacks.
- `usbd_conf.c`: includes and `usb_cdc_port_configure_irq()` in
  USB_OTG_FS_MspInit 1. It sets priority 6 and keeps the IRQ disabled until the
  TX task calls `activate()` after the scheduler starts. MspInit 0 enables
  `vbus_sensing_enable` before HAL configures the USB core. MspInit 1 configures
  PA9 as an input without pulls; MspDeInit 1 releases the pin.
- `usbd_desc.c`: the shared identity header in INCLUDE and USB string overrides
  in PRIVATE_DEFINES. Regeneration preserves the device name.
- Headers: declarations and includes inside their corresponding user sections.

The `core/`, `platform/`, `tests/` and `qemu/` directories and `GNUmakefile` are
maintained separately. Plain `make` uses `GNUmakefile` rather than the Makefile
that CubeMX can overwrite. The project is verified with its existing `Src/Inc`
layout and STM32Cube FW_F4 V1.24.2 libraries. A CubeMX/HAL update that changes
the directory layout requires separate build and USB API validation.

The FreeRTOS kernel and ARM_CM4F port are in `third_party/FreeRTOS-Kernel/`;
configuration and hooks are in `platform/freertos/`. Do not add another copy of
the kernel or CMSIS-RTOS through the generator. SVC and PendSV must enter the
port handlers directly, without ordinary C wrappers, because they operate on
the interrupted task's stack. USER CODE Includes renames the empty generated
handlers so the vector table resolves to the FreeRTOS handlers. SysTick retains
`HAL_IncTick()` and calls RTOS support only after the scheduler starts.

After creating static tasks, board code restores the original BASEPRI: USB
initialization uses HAL delays and needs a running SysTick. This does not allow
early USB callbacks; the USB IRQ stays disabled until `activate()`.

The `.ioc` assigns PA9 to `USB_OTG_FS_VBUS` in `Activate_VBUS` mode and sets
`USB_OTG_FS.vbus_sensing_enable` to `ENABLE`. PA9 must be connected to the USB FS
connector's VBUS so the device detects cable removal while powered separately.
The MSP sections preserve this configuration across regeneration. If CubeMX
also generates PA9 configuration, applying input mode a second time is safe.
The native check in `tests/test_usb_vbus.py` executes the LL/MSP functions with
HAL stubs and verifies that sensing and the PA9 input are ready before USB core
configuration. It also checks that endpoints close before the CDC
deinitialization callback. The physical VBUS circuit and cable removal behavior
are checked [on hardware](hardware-validation.md).

```sh
make check-cubemx
```

This check removes USER CODE contents from the files listed in
`tests/cubemx_generated.sha256.json` and compares the SHA256 hashes of the
remaining text with the approved baseline. It also checks marker pairing. The
test does not run CubeMX or prove compatibility with arbitrary future generator
versions.

To regenerate the project:

1. Save working changes in Git and verify that the `.ioc` is up to date.
2. Generate the project with Keep User Code enabled.
3. Review the diff. Integration points must remain inside USER CODE sections,
   handler definitions must not be duplicated, SVC/PendSV must resolve to the
   FreeRTOS port, and SysTick must service both HAL and the RTOS.
4. If the generator intentionally changes text outside those sections, review
   each difference before updating its SHA256 in the baseline.
5. Run `make docker-build`. Also check USB and clock changes
   [on hardware](hardware-validation.md).

Never update the baseline automatically during verification: doing so would
hide changes outside the permitted sections.

Do not assign CubeMX the TIM1/TIM8/DMA/EXTI handlers owned by the adapter.
TIM6_DAC is reserved for software capture notifications at priority 5; its
handler calls `capture_task_events_irq()`. That IRQ cannot also serve another
timer or the DAC. If you move configuration of these resources into the `.ioc`,
first remove or move the corresponding adapters into the USER CODE sections
created by CubeMX to avoid duplicate definitions. This is not needed when
regenerating the current `.ioc`.
