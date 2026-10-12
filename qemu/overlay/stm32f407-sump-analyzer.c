/*
 * STM32F407 analyzer test machine: the subset used by this firmware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixed 168 MHz timer clock; no USB, PLL startup, bus contention or analog
 * timing. The stock netduinoplus2 machine is unaffected. Registers follow
 * RM0090; the test fixture at 0x60000000 is intentionally not STM32 hardware.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "hw/arm/boot.h"
#include "hw/arm/stm32f405_soc.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"

#define TYPE_ANALYZER "stm32f407-sump-analyzer-peripherals"
OBJECT_DECLARE_SIMPLE_TYPE(AnalyzerState, ANALYZER)

#define TIMER_CLOCK_HZ 168000000u
#define GPIOD_BASE 0x40020c00u
#define TIM1_BASE 0x40010000u
#define TIM8_BASE 0x40010400u
#define DMA2_BASE 0x40026400u
#define FIXTURE_BASE 0x60000000u
#define FIXTURE_SIGNATURE 0x534c4131u /* SLA1 */
#define GPIO_PIN_MASK 0xffffu

enum { GPIO, TIM1, TIM8, DMA2, FIXTURE, REGION_COUNT };

/* Register indices are byte offsets divided by the 32-bit register width. */
enum {
    GPIO_IDR = 4,
    GPIO_ODR = 5,
    GPIO_BSRR = 6,
};
enum {
    CR1 = 0, CR2 = 1, SMCR = 2, DIER = 3, SR = 4, EGR = 5,
    CNT = 9, PSC = 10, ARR = 11,
};
enum {
    DMA_LISR = 0, DMA_HISR = 1, DMA_LIFCR = 2, DMA_HIFCR = 3,
    DMA_STREAM5 = 0x88 / 4,
};
enum { STREAM_CR, STREAM_NDTR, STREAM_PAR, STREAM_M0AR };
enum {
    FIXTURE_ID, FIXTURE_LEVEL, FIXTURE_STEP, FIXTURE_AUTO_INCREMENT,
    FIXTURE_TICKS, FIXTURE_DMA_ERROR,
};

enum {
    TIM_CEN = 1 << 0,
    TIM_URS = 1 << 2,
    TIM_OPM = 1 << 3,
    TIM_UIF = 1 << 0,
    TIM_UG = 1 << 0,
    TIM_UIE = 1 << 0,
    TIM_UDE = 1 << 8,
    TIM_MMS_MASK = 7 << 4,
    TIM_MMS_UPDATE = 2 << 4,
    TIM_SLAVE_MODE_MASK = 7,
    TIM_TRIGGER_INPUT_MASK = 7 << 4,
    TIM_EXTERNAL_CLOCK_ITR0 = 7,
};
enum {
    DMA_EN = 1 << 0,
    DMA_TEIE = 1 << 2,
    DMA_TCIE = 1 << 4,
    DMA_DIRECTION_MASK = 3 << 6,
    DMA_CIRC = 1 << 8,
    DMA_PINC = 1 << 9,
    DMA_MINC = 1 << 10,
    DMA_PERIPHERAL_SIZE_MASK = 3 << 11,
    DMA_MEMORY_SIZE_MASK = 3 << 13,
    DMA_CHANNEL_MASK = 7 << 25,
    DMA_CHANNEL6 = 6 << 25,
    DMA_TE = 1 << 9,
    DMA_HT = 1 << 10,
    DMA_TC = 1 << 11,
};
enum { TIM8_UPDATE_IRQ = 44, DMA2_STREAM5_IRQ = 68, SYSCFG_PORT_D = 3 };

typedef struct AnalyzerRegion {
    AnalyzerState *s;
    unsigned kind;
    MemoryRegion memory;
} AnalyzerRegion;

struct AnalyzerState {
    SysBusDevice parent_obj;
    AnalyzerRegion regions[REGION_COUNT];
    QEMUTimer *timer;
    qemu_irq timer_irq, dma_irq, pins[16];
    uint32_t gpio[10], tim1[20], tim8[20], dma[0x100 / 4];
    uint32_t initial_count, transferred;
    uint32_t fixture_step, fixture_enable, fixture_ticks;
    bool inject_error;
};

static uint32_t *dma_stream5(AnalyzerState *s)
{
    return &s->dma[DMA_STREAM5];
}

static void update_dma_irq(AnalyzerState *s)
{
    uint32_t cr = dma_stream5(s)[STREAM_CR];
    uint32_t status = s->dma[DMA_HISR];
    qemu_set_irq(s->dma_irq, ((status & DMA_TC) && (cr & DMA_TCIE)) ||
                            ((status & DMA_TE) && (cr & DMA_TEIE)));
}

static void update_timer_irq(AnalyzerState *s)
{
    qemu_set_irq(s->timer_irq, (s->tim8[SR] & TIM_UIF) && (s->tim8[DIER] & TIM_UIE));
}

static void gpio_input(AnalyzerState *s, uint32_t value)
{
    uint32_t changed = s->gpio[GPIO_IDR] ^ (value & GPIO_PIN_MASK);
    s->gpio[GPIO_IDR] = value & GPIO_PIN_MASK;
    for (unsigned i = 0; i < 16; ++i) {
        if (changed & (1u << i)) {
            qemu_set_irq(s->pins[i], (value >> i) & 1);
        }
    }
}

static void dma_transfer_error(AnalyzerState *s)
{
    dma_stream5(s)[STREAM_CR] &= ~DMA_EN;
    s->dma[DMA_HISR] |= DMA_TE;
    update_dma_irq(s);
}

static void dma_request(AnalyzerState *s)
{
    uint32_t *stream = dma_stream5(s);
    uint32_t control = stream[STREAM_CR];
    uint32_t unsupported = DMA_DIRECTION_MASK | DMA_PINC |
                           DMA_PERIPHERAL_SIZE_MASK | DMA_MEMORY_SIZE_MASK;

    if (!(control & DMA_EN)) {
        return;
    }
    /* TIM1_UP uses DMA2 stream 5/channel 6. Each request reads one byte from
     * the fixed GPIO input address; peripheral increment is invalid here. */
    if (s->inject_error || !stream[STREAM_NDTR] ||
        (control & DMA_CHANNEL_MASK) != DMA_CHANNEL6 ||
        (control & unsupported) ||
        stream[STREAM_PAR] != GPIOD_BASE + GPIO_IDR * sizeof(uint32_t)) {
        s->inject_error = false;
        dma_transfer_error(s);
        return;
    }

    uint8_t sample = s->gpio[GPIO_IDR];
    hwaddr address = stream[STREAM_M0AR] +
                     ((control & DMA_MINC) ? s->transferred : 0);
    if (address_space_write(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED,
                            &sample, 1) != MEMTX_OK) {
        dma_transfer_error(s);
        return;
    }

    ++s->transferred;
    --stream[STREAM_NDTR];
    if (stream[STREAM_NDTR] == s->initial_count / 2) {
        s->dma[DMA_HISR] |= DMA_HT;
    }
    if (!stream[STREAM_NDTR]) {
        s->dma[DMA_HISR] |= DMA_TC;
        if (control & DMA_CIRC) {
            stream[STREAM_NDTR] = s->initial_count;
            s->transferred = 0;
        } else {
            stream[STREAM_CR] &= ~DMA_EN;
        }
    }
    update_dma_irq(s);
}

static int64_t sample_interval_ns(AnalyzerState *s)
{
    uint64_t ticks = ((uint64_t)s->tim1[PSC] + 1) * ((uint64_t)s->tim1[ARR] + 1);
    return MAX(1, (ticks * 1000000000ULL + TIMER_CLOCK_HZ - 1) / TIMER_CLOCK_HZ);
}

static void schedule_sample(AnalyzerState *s)
{
    timer_del(s->timer);
    if (s->tim1[CR1] & TIM_CEN) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            sample_interval_ns(s));
    }
}

static void timer_tick(void *opaque)
{
    AnalyzerState *s = opaque;
    if (!(s->tim1[CR1] & TIM_CEN)) {
        return;
    }
    ++s->fixture_ticks;
    if (s->fixture_enable) {
        gpio_input(s, s->gpio[GPIO_IDR] + s->fixture_step);
    }
    s->tim1[CNT] = 0;
    s->tim1[SR] |= TIM_UIF;
    if (s->tim1[DIER] & TIM_UDE) {
        dma_request(s);
    }
    /* TIM1 MMS=update -> TIM8 ITR0 external clock mode 1. */
    if ((s->tim1[CR2] & TIM_MMS_MASK) == TIM_MMS_UPDATE &&
        (s->tim8[CR1] & TIM_CEN) &&
        (s->tim8[SMCR] & (TIM_SLAVE_MODE_MASK | TIM_TRIGGER_INPUT_MASK)) ==
            TIM_EXTERNAL_CLOCK_ITR0) {
        if (++s->tim8[CNT] > s->tim8[ARR]) {
            s->tim8[CNT] = 0;
            s->tim8[SR] |= TIM_UIF;
            if (s->tim8[CR1] & TIM_OPM) {
                s->tim8[CR1] &= ~TIM_CEN;
            }
            update_timer_irq(s);
        }
    }
    if (s->tim1[CR1] & TIM_OPM) {
        s->tim1[CR1] &= ~TIM_CEN;
    }
    schedule_sample(s);
}

static uint64_t analyzer_read(void *opaque, hwaddr addr, unsigned size)
{
    AnalyzerRegion *r = opaque;
    AnalyzerState *s = r->s;
    uint32_t value = 0;
    unsigned reg = addr / 4;
    switch (r->kind) {
    case GPIO:
        if (reg < ARRAY_SIZE(s->gpio)) {
            value = s->gpio[reg];
        }
        break;
    case TIM1:
        if (reg < ARRAY_SIZE(s->tim1)) {
            value = s->tim1[reg];
        }
        break;
    case TIM8:
        if (reg < ARRAY_SIZE(s->tim8)) {
            value = s->tim8[reg];
        }
        break;
    case DMA2:
        if (reg < ARRAY_SIZE(s->dma)) {
            value = s->dma[reg];
        }
        break;
    case FIXTURE:
        switch (reg) {
        case FIXTURE_ID:
            value = FIXTURE_SIGNATURE;
            break;
        case FIXTURE_LEVEL:
            value = s->gpio[GPIO_IDR];
            break;
        case FIXTURE_STEP:
            value = s->fixture_step;
            break;
        case FIXTURE_AUTO_INCREMENT:
            value = s->fixture_enable;
            break;
        case FIXTURE_TICKS:
            value = s->fixture_ticks;
            break;
        case FIXTURE_DMA_ERROR:
            value = s->inject_error;
            break;
        }
        break;
    }
    return value >> ((addr & 3) * 8);
}

static void analyzer_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    AnalyzerRegion *r = opaque;
    AnalyzerState *s = r->s;
    unsigned reg = addr / 4;
    uint32_t v = value;
    if (size != 4 || (addr & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR, "analyzer: requires aligned 32-bit register writes\n");
        return;
    }
    switch (r->kind) {
    case GPIO:
        if (reg == GPIO_IDR) {
            return; /* Input data is read-only. */
        }
        if (reg == GPIO_BSRR) {
            s->gpio[GPIO_ODR] = (s->gpio[GPIO_ODR] | (v & GPIO_PIN_MASK)) &
                                ~(v >> 16);
            return;
        }
        if (reg < ARRAY_SIZE(s->gpio)) {
            s->gpio[reg] = v;
        }
        break;
    case TIM1:
    case TIM8: {
        uint32_t *timer = r->kind == TIM1 ? s->tim1 : s->tim8;
        if (reg >= ARRAY_SIZE(s->tim1)) {
            return;
        }
        if (reg == SR) {
            timer[SR] &= v; /* Status bits clear when zero is written. */
        } else if (reg == EGR) {
            if (v & TIM_UG) {
                timer[CNT] = 0;
                if (!(timer[CR1] & TIM_URS)) {
                    timer[SR] |= TIM_UIF;
                }
            }
        } else {
            bool is_counter_register = reg == PSC || reg == ARR || reg == CNT;
            timer[reg] = is_counter_register ? v & 0xffffu : v;
        }
        if (r->kind == TIM1 &&
            (reg == CR1 || reg == PSC || reg == ARR || reg == EGR)) {
            schedule_sample(s);
        }
        if (r->kind == TIM8) {
            update_timer_irq(s);
        }
        break;
    }
    case DMA2:
        if (reg >= ARRAY_SIZE(s->dma) || reg == DMA_LISR || reg == DMA_HISR) {
            return;
        }
        if (reg == DMA_LIFCR || reg == DMA_HIFCR) {
            /* Each clear register maps to its matching status register. */
            s->dma[reg - DMA_LIFCR] &= ~v;
        } else {
            if (reg == DMA_STREAM5 && !(s->dma[reg] & DMA_EN) && (v & DMA_EN)) {
                s->initial_count = dma_stream5(s)[STREAM_NDTR];
                s->transferred = 0;
            }
            s->dma[reg] = v;
        }
        update_dma_irq(s);
        break;
    case FIXTURE:
        switch (reg) {
        case FIXTURE_LEVEL:
            gpio_input(s, v);
            break;
        case FIXTURE_STEP:
            s->fixture_step = v;
            break;
        case FIXTURE_AUTO_INCREMENT:
            s->fixture_enable = v & 1;
            break;
        case FIXTURE_TICKS:
            s->fixture_ticks = 0;
            break;
        case FIXTURE_DMA_ERROR:
            s->inject_error = v != 0;
            break;
        }
        break;
    }
}

static const MemoryRegionOps analyzer_ops = {
    .read = analyzer_read, .write = analyzer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .impl.min_access_size = 1, .impl.max_access_size = 4,
};

static void analyzer_reset(DeviceState *dev)
{
    AnalyzerState *s = ANALYZER(dev);
    timer_del(s->timer);
    memset(s->gpio, 0, sizeof(s->gpio));
    memset(s->tim1, 0, sizeof(s->tim1));
    memset(s->tim8, 0, sizeof(s->tim8));
    memset(s->dma, 0, sizeof(s->dma));
    s->tim1[ARR] = s->tim8[ARR] = 0xffff;
    s->initial_count = s->transferred = 0;
    s->fixture_step = s->fixture_enable = s->fixture_ticks = 0;
    s->inject_error = false;
    qemu_set_irq(s->timer_irq, 0);
    qemu_set_irq(s->dma_irq, 0);
    for (unsigned i = 0; i < 16; ++i) {
        qemu_set_irq(s->pins[i], 0);
    }
}

static void analyzer_init(Object *obj)
{
    AnalyzerState *s = ANALYZER(obj);
    static const char *const names[] = {"analyzer.gpiod", "analyzer.tim1",
        "analyzer.tim8", "analyzer.dma2", "analyzer.fixture"};
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_tick, s);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->timer_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->dma_irq);
    qdev_init_gpio_out(DEVICE(obj), s->pins, 16);
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        s->regions[i].s = s;
        s->regions[i].kind = i;
        memory_region_init_io(&s->regions[i].memory, obj, &analyzer_ops,
                               &s->regions[i], names[i], 0x400);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regions[i].memory);
    }
}

static void analyzer_finalize(Object *obj)
{
    timer_free(ANALYZER(obj)->timer);
}

static const VMStateDescription analyzer_vmstate = {
    .name = TYPE_ANALYZER, .unmigratable = 1,
};
static void analyzer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, analyzer_reset);
    dc->vmsd = &analyzer_vmstate;
}
static const TypeInfo analyzer_type = {
    .name = TYPE_ANALYZER, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AnalyzerState), .instance_init = analyzer_init,
    .instance_finalize = analyzer_finalize, .class_init = analyzer_class_init,
};
static void analyzer_register(void)
{
    type_register_static(&analyzer_type);
}
type_init(analyzer_register)

static void analyzer_machine_init(MachineState *machine)
{
    Clock *sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, TIMER_CLOCK_HZ);
    DeviceState *socdev = qdev_new(TYPE_STM32F405_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(socdev));
    qdev_connect_clock_in(socdev, "sysclk", sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(socdev), &error_fatal);
    STM32F405State *soc = STM32F405_SOC(socdev);
    DeviceState *dev = qdev_new(TYPE_ANALYZER);
    object_property_add_child(OBJECT(machine), "analyzer", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    const hwaddr addresses[] = {
        [GPIO] = GPIOD_BASE,
        [TIM1] = TIM1_BASE,
        [TIM8] = TIM8_BASE,
        [DMA2] = DMA2_BASE,
        [FIXTURE] = FIXTURE_BASE,
    };
    for (unsigned i = 0; i < REGION_COUNT; ++i) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), i, addresses[i], 1);
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
        qdev_get_gpio_in(DEVICE(&soc->armv7m), TIM8_UPDATE_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
        qdev_get_gpio_in(DEVICE(&soc->armv7m), DMA2_STREAM5_IRQ));
    for (unsigned i = 0; i < 16; ++i) {
        qdev_connect_gpio_out(dev, i,
            qdev_get_gpio_in(DEVICE(&soc->syscfg), SYSCFG_PORT_D * 16 + i));
    }
    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, 0, FLASH_SIZE);
}
static void analyzer_machine_class_init(MachineClass *mc)
{
    mc->desc = "STM32F407 analyzer capture test machine (GPIO/DMA/TIM1/TIM8 subset)";
    mc->init = analyzer_machine_init;
}
DEFINE_MACHINE("stm32f407-sump-analyzer", analyzer_machine_class_init)
