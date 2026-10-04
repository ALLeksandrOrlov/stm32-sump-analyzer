# Hand-maintained entry point. GNU make prefers this to the CubeMX Makefile.
.DEFAULT_GOAL := all
BUILD_DIR ?= out
TARGET := stm32_sump_analyzer
CROSS_COMPILE ?= arm-none-eabi-
ARM_CC := $(CROSS_COMPILE)gcc
ARM_OBJCOPY := $(CROSS_COMPILE)objcopy
ARM_SIZE := $(CROSS_COMPILE)size
HOST_CC ?= cc
PYTHON ?= python3
QEMU ?= qemu-system-arm
CORE := core/sump_protocol.c core/capture.c core/analyzer_app.c
HAL_DIR := Drivers/STM32F4xx_HAL_Driver
USB_DIR := Middlewares/ST/STM32_USB_Device_Library
HAL_NAMES := hal hal_cortex hal_dma hal_dma_ex hal_exti hal_flash hal_flash_ex \
 hal_flash_ramfunc hal_gpio hal_pcd hal_pcd_ex hal_pwr hal_pwr_ex hal_rcc \
 hal_rcc_ex hal_tim hal_tim_ex ll_usb
FIRMWARE := $(CORE) $(wildcard Src/*.c) $(wildcard platform/stm32/*.c) \
 $(addprefix $(HAL_DIR)/Src/stm32f4xx_,$(addsuffix .c,$(HAL_NAMES))) \
 $(addprefix $(USB_DIR)/Core/Src/,usbd_core.c usbd_ctlreq.c usbd_ioreq.c) $(USB_DIR)/Class/CDC/Src/usbd_cdc.c
INCLUDES := -Icore -Iplatform/stm32 -IInc -I$(HAL_DIR)/Inc \
 -I$(HAL_DIR)/Inc/Legacy -IDrivers/CMSIS/Include \
 -IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
 -I$(USB_DIR)/Core/Inc -I$(USB_DIR)/Class/CDC/Inc
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
FW_CFLAGS := $(CPU) -std=c11 -Os -g3 -Wall -Wextra -fno-common \
 -ffunction-sections -fdata-sections -DUSE_HAL_DRIVER -DSTM32F407xx $(INCLUDES)
FW_LDFLAGS := $(CPU) -TSTM32F407VGTx_FLASH.ld --specs=nano.specs \
 --specs=nosys.specs -Wl,--gc-sections,-Map=$(BUILD_DIR)/firmware/$(TARGET).map
OBJECTS := $(addprefix $(BUILD_DIR)/firmware/,$(FIRMWARE:.c=.o)) \
 $(BUILD_DIR)/firmware/startup_stm32f407xx.o
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -pedantic -Icore
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_SOURCES := tests/test_core.c $(CORE)
QEMU_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -std=c11 -Os -g \
 -Wall -Wextra -Werror -Icore -ffunction-sections -fdata-sections
.PHONY: all firmware test test-sanitize check-cubemx qemu-test clean docker-build docker-test
all: firmware
firmware: $(BUILD_DIR)/firmware/$(TARGET).elf $(BUILD_DIR)/firmware/$(TARGET).hex $(BUILD_DIR)/firmware/$(TARGET).bin
$(BUILD_DIR)/firmware/%.o: %.c GNUmakefile
	@mkdir -p $(@D)
	$(ARM_CC) $(FW_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/firmware/%.o: %.s GNUmakefile
	@mkdir -p $(@D)
	$(ARM_CC) $(CPU) -c $< -o $@
$(BUILD_DIR)/firmware/$(TARGET).elf: $(OBJECTS) STM32F407VGTx_FLASH.ld
	$(ARM_CC) $(OBJECTS) $(FW_LDFLAGS) -o $@
	$(ARM_SIZE) $@
$(BUILD_DIR)/firmware/%.hex: $(BUILD_DIR)/firmware/%.elf
	$(ARM_OBJCOPY) -O ihex $< $@
$(BUILD_DIR)/firmware/%.bin: $(BUILD_DIR)/firmware/%.elf
	$(ARM_OBJCOPY) -O binary $< $@
$(BUILD_DIR)/tests/test_core: $(TEST_SOURCES) $(wildcard core/*.h) GNUmakefile
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_FLAGS) $(TEST_SOURCES) -o $@
test: check-cubemx $(BUILD_DIR)/tests/test_core
	$(BUILD_DIR)/tests/test_core
test-sanitize: check-cubemx
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) $(TEST_SOURCES) -o $(BUILD_DIR)/tests/test_core_sanitize
	$(BUILD_DIR)/tests/test_core_sanitize
check-cubemx:
	$(PYTHON) scripts/check_cubemx.py
$(BUILD_DIR)/qemu/test_core.elf: $(TEST_SOURCES) $(wildcard core/*.h) tests/qemu/startup.c tests/qemu/memory.ld GNUmakefile
	@mkdir -p $(@D)
	$(ARM_CC) $(QEMU_FLAGS) $(TEST_SOURCES) tests/qemu/startup.c -nostartfiles \
	 --specs=rdimon.specs -Ttests/qemu/memory.ld -Wl,--gc-sections -o $@
qemu-test: $(BUILD_DIR)/qemu/test_core.elf
	$(PYTHON) scripts/run_qemu.py --qemu $(QEMU) --elf $<
# Both aliases run the complete pipeline; release artifacts require all tests.
docker-build docker-test:
	sh scripts/docker-ci.sh
clean:
	rm -rf $(BUILD_DIR)
-include $(OBJECTS:.o=.d)

# Requires the QEMU overlay from qemu/ (the stock machine lacks these devices).
QEMU_STM32_SOURCES := tests/qemu/test_capture_stm32.c tests/qemu/startup.c \
 $(CORE) platform/stm32/capture_stm32.c \
 $(HAL_DIR)/Src/stm32f4xx_hal_gpio.c $(HAL_DIR)/Src/stm32f4xx_hal_cortex.c
$(BUILD_DIR)/qemu/test_capture_stm32.elf: $(QEMU_STM32_SOURCES) $(wildcard core/*.h) platform/stm32/capture_stm32.h tests/qemu/memory.ld GNUmakefile
	@mkdir -p $(@D)
	$(ARM_CC) $(QEMU_FLAGS) $(INCLUDES) -DSTM32F407xx -DUSE_HAL_DRIVER \
	 -DSTM32_CAPTURE_TEST $(QEMU_STM32_SOURCES) -nostartfiles --specs=rdimon.specs \
	 -Ttests/qemu/memory.ld -Wl,--gc-sections -o $@
.PHONY: qemu-stm32-test
qemu-stm32-test: $(BUILD_DIR)/qemu/test_capture_stm32.elf
	$(PYTHON) scripts/run_qemu.py --qemu $(QEMU) --elf $< \
	 --machine stm32f407-sump-analyzer --success 'ALL STM32 CAPTURE TESTS PASSED'

BOARD_TEST_SOURCES := tests/test_board.c platform/stm32/analyzer_board.c $(CORE)
$(BUILD_DIR)/tests/test_board: $(BOARD_TEST_SOURCES) $(wildcard core/*.h) $(wildcard platform/stm32/*.h) tests/stubs/usbd_cdc_if.h GNUmakefile
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_FLAGS) -Itests/stubs -Iplatform/stm32 $(BOARD_TEST_SOURCES) -o $@
.PHONY: test-board test-board-sanitize
test-board: $(BUILD_DIR)/tests/test_board
	$<
test: test-board
test-board-sanitize:
	@mkdir -p $(BUILD_DIR)/tests
	$(HOST_CC) $(HOST_FLAGS) $(SANITIZERS) -Itests/stubs -Iplatform/stm32 $(BOARD_TEST_SOURCES) -o $(BUILD_DIR)/tests/test_board_sanitize
	$(BUILD_DIR)/tests/test_board_sanitize
test-sanitize: test-board-sanitize
