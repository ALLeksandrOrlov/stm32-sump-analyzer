"""Execute the generated USB setup and vendor CDC shutdown with observable ports.

The harness compiles the real function bodies; it does not emulate OTG registers
or claim that a software test can verify the board's physical VBUS connection.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[1]


def function_body(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"(?:static\s+)?(?:void|uint8_t|HAL_StatusTypeDef|USBD_StatusTypeDef)\s+" +
                      re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"Function {name} not found in {path}")
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class UsbVbusTests(unittest.TestCase):
    def run_c(self, source):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a native C compiler is required for USB setup tests")
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "test.c"
            executable = Path(directory) / "test"
            source_path.write_text(source)
            compiled = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", str(source_path), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(executable)], check=True, timeout=5)

    def test_ll_initialization_enables_sensing_and_configures_pa9_before_core_init(self):
        functions = "\n".join(function_body("Src/usbd_conf.c", name) for name in
                              ("HAL_PCD_MspInit", "HAL_PCD_MspDeInit", "USBD_LL_Init"))
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define ENABLE 1
#define DISABLE 0
#define HAL_OK 0
#define USBD_OK 0
#define DEVICE_FS 0
#define USE_HAL_PCD_REGISTER_CALLBACKS 0
#define PCD_SPEED_FULL 1
#define PCD_PHY_EMBEDDED 1
#define GPIO_PIN_9 (1u << 9)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_AF_PP 2
#define GPIO_NOPULL 0
#define GPIO_SPEED_FREQ_VERY_HIGH 3
#define GPIO_AF10_OTG_FS 10
#define OTG_FS_IRQn 67
static int usb_instance, gpio_port;
#define USB_OTG_FS (&usb_instance)
#define GPIOA (&gpio_port)
typedef int HAL_StatusTypeDef;
typedef int USBD_StatusTypeDef;
typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;
typedef struct {
    uint32_t dev_endpoints, speed, dma_enable, phy_itface, Sof_enable;
    uint32_t low_power_enable, lpm_enable, vbus_sensing_enable, use_dedicated_ep1;
} PCD_InitTypeDef;
typedef struct { void *Instance, *pData; PCD_InitTypeDef Init; } PCD_HandleTypeDef;
typedef struct { int id; void *pData; } USBD_HandleTypeDef;
PCD_HandleTypeDef hpcd_USB_OTG_FS;
static int gpio_clock, usb_clock, irq_enabled, configured_irq;
static uint32_t initialized_pins, deinitialized_pins;
#define __HAL_RCC_GPIOA_CLK_ENABLE() (gpio_clock = 1)
#define __HAL_RCC_USB_OTG_FS_CLK_ENABLE() (usb_clock = 1)
#define __HAL_RCC_USB_OTG_FS_CLK_DISABLE() (usb_clock = 0)
static void HAL_GPIO_Init(void *port, GPIO_InitTypeDef *config) {
    assert(port == GPIOA && gpio_clock);
    initialized_pins |= config->Pin;
    if (config->Pin == GPIO_PIN_9) {
        assert(config->Mode == GPIO_MODE_INPUT && config->Pull == GPIO_NOPULL);
    }
}
static void HAL_GPIO_DeInit(void *port, uint32_t pins) {
    assert(port == GPIOA);
    deinitialized_pins |= pins;
}
static void HAL_NVIC_SetPriority(int irq, int priority, int subpriority) {
    assert(irq == OTG_FS_IRQn);
    (void)priority; (void)subpriority;
}
static void HAL_NVIC_EnableIRQ(int irq) {
    assert(irq == OTG_FS_IRQn); irq_enabled = 1;
}
static void HAL_NVIC_DisableIRQ(int irq) {
    assert(irq == OTG_FS_IRQn); irq_enabled = 0;
}
static void usb_cdc_port_configure_irq(void) {
    irq_enabled = 0; configured_irq = 1;
}
void HAL_PCD_MspInit(PCD_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_PCD_Init(PCD_HandleTypeDef *handle) {
    /* HAL_PCD_Init invokes MSP before USB_CoreInit consumes Init. */
    HAL_PCD_MspInit(handle);
    assert(handle->Init.vbus_sensing_enable == ENABLE);
    assert((initialized_pins & GPIO_PIN_9) != 0);
    assert(gpio_clock && usb_clock && configured_irq && !irq_enabled);
    return HAL_OK;
}
static void HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *handle, int size) {
    (void)handle; (void)size;
}
static void HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *handle, int fifo, int size) {
    (void)handle; (void)fifo; (void)size;
}
static void Error_Handler(void) { abort(); }
''' + functions + r'''
int main(void) {
    PCD_HandleTypeDef unrelated = {0};
    HAL_PCD_MspInit(&unrelated);
    assert(!gpio_clock && !usb_clock && initialized_pins == 0);
    USBD_HandleTypeDef device = {.id = DEVICE_FS};
    assert(USBD_LL_Init(&device) == USBD_OK);
    assert(device.pData == &hpcd_USB_OTG_FS && hpcd_USB_OTG_FS.pData == &device);
    HAL_PCD_MspDeInit(&hpcd_USB_OTG_FS);
    assert((deinitialized_pins & GPIO_PIN_9) != 0);
    assert(!usb_clock && !irq_enabled);
}
''')

    def test_hal_calls_msp_before_consuming_configuration(self):
        body = function_body("Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pcd.c",
                             "HAL_PCD_Init")
        self.assertLess(body.index("HAL_PCD_MspInit(hpcd)"), body.index("USB_CoreInit("))

    def test_ioc_selects_physical_vbus_input_and_sensing(self):
        config = dict(line.split("=", 1) for line in
                      (ROOT / "stm32_sump_analyzer.ioc").read_text().splitlines() if "=" in line)
        self.assertEqual(config["PA9.Mode"], "Activate_VBUS")
        self.assertEqual(config["PA9.Signal"], "USB_OTG_FS_VBUS")
        self.assertEqual(config["USB_OTG_FS.vbus_sensing_enable"], "ENABLE")
        self.assertIn("vbus_sensing_enable", config["USB_OTG_FS.IPParameters"].split(","))
        pins = [config[f"Mcu.Pin{index}"] for index in range(int(config["Mcu.PinsNb"]))]
        self.assertEqual(pins.count("PA9"), 1)

    def test_disconnect_closes_endpoints_before_releasing_the_interface(self):
        functions = function_body(
            "Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c", "USBD_CDC_DeInit")
        functions += "\n" + function_body(
            "Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c", "USBD_LL_DevDisconnected")
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define CDC_IN_EP 0x81u
#define CDC_OUT_EP 0x01u
#define CDC_CMD_EP 0x82u
#define USBD_STATE_DEFAULT 1
#define USBD_OK 0
typedef int USBD_StatusTypeDef;
typedef struct Device USBD_HandleTypeDef;
typedef struct { uint8_t (*DeInit)(USBD_HandleTypeDef *, uint8_t); } Class;
typedef struct { int8_t (*DeInit)(void); } USBD_CDC_ItfTypeDef;
struct Device {
    int dev_state, dev_config;
    Class *pClass;
    void *pClassData, *pUserData;
    struct { int is_used; } ep_in[16], ep_out[16];
};
static unsigned closed, notified, freed;
static void USBD_LL_CloseEP(USBD_HandleTypeDef *device, unsigned endpoint) {
    assert(device->dev_state == USBD_STATE_DEFAULT && !notified && !freed);
    if (endpoint == CDC_IN_EP) closed |= 1;
    else if (endpoint == CDC_OUT_EP) closed |= 2;
    else if (endpoint == CDC_CMD_EP) closed |= 4;
    else assert(0);
}
static int8_t interface_deinit(void) {
    assert(closed == 7 && !freed);
    ++notified;
    return 0;
}
static void USBD_free(void *data) {
    assert(data != NULL && closed == 7 && notified == 1);
    ++freed;
}
''' + functions + r'''
int main(void) {
    Class cdc_class = {.DeInit = USBD_CDC_DeInit};
    USBD_CDC_ItfTypeDef interface = {.DeInit = interface_deinit};
    int class_data;
    USBD_HandleTypeDef device = {
        .pClass = &cdc_class, .pClassData = &class_data, .pUserData = &interface
    };
    assert(USBD_LL_DevDisconnected(&device) == USBD_OK);
    assert(notified == 1 && freed == 1 && device.pClassData == NULL);
    assert(device.ep_in[1].is_used == 0 && device.ep_in[2].is_used == 0);
    assert(device.ep_out[1].is_used == 0);
}
''')


if __name__ == "__main__":
    unittest.main()
