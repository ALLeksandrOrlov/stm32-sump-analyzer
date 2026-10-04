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

enum { GPIO, TIM1, TIM8, DMA2, FIXTURE, REGION_COUNT };
enum { CR1 = 0, CR2 = 1, SMCR = 2, DIER = 3, SR = 4, EGR = 5,
       CNT = 9, PSC = 10, ARR = 11 };
enum { DMA_EN = 1, DMA_TCIE = 1 << 4, DMA_TEIE = 1 << 2,
       DMA_CIRC = 1 << 8, DMA_MINC = 1 << 10,
       DMA_TC = 1 << 11, DMA_HT = 1 << 10, DMA_TE = 1 << 9 };

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

static uint32_t *stream(AnalyzerState *s) { return &s->dma[0x88 / 4]; }

static void update_dma_irq(AnalyzerState *s)
{
    uint32_t cr = stream(s)[0], status = s->dma[1];
    qemu_set_irq(s->dma_irq, ((status & DMA_TC) && (cr & DMA_TCIE)) ||
                            ((status & DMA_TE) && (cr & DMA_TEIE)));
}

static void update_timer_irq(AnalyzerState *s)
{
    qemu_set_irq(s->timer_irq, (s->tim8[SR] & s->tim8[DIER] & 1) != 0);
}

static void gpio_input(AnalyzerState *s, uint32_t value)
{
    uint32_t changed = s->gpio[4] ^ (value & 0xffff);
    s->gpio[4] = value & 0xffff;
    for (unsigned i = 0; i < 16; ++i) {
        if (changed & (1u << i)) qemu_set_irq(s->pins[i], (value >> i) & 1);
    }
}

static void dma_request(AnalyzerState *s)
{
    uint32_t *d = stream(s);
    if (!(d[0] & DMA_EN)) return;
    /* TIM1_UP is DMA2/stream5/channel6; only peripheral-to-memory byte mode. */
    if (s->inject_error || !d[1] || ((d[0] >> 25) & 7) != 6 ||
        (d[0] & ((3u << 6) | (3u << 11) | (3u << 13))) ||
        d[2] != 0x40020c10) {
        s->inject_error = false;
        d[0] &= ~DMA_EN;
        s->dma[1] |= DMA_TE;
        update_dma_irq(s);
        return;
    }
    uint8_t sample = s->gpio[4];
    hwaddr address = d[3] + ((d[0] & DMA_MINC) ? s->transferred : 0);
    if (address_space_write(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED,
                            &sample, 1) != MEMTX_OK) {
        d[0] &= ~DMA_EN;
        s->dma[1] |= DMA_TE;
    } else {
        ++s->transferred;
        --d[1];
        if (d[1] == s->initial_count / 2) s->dma[1] |= DMA_HT;
        if (!d[1]) {
            s->dma[1] |= DMA_TC;
            if (d[0] & DMA_CIRC) {
                d[1] = s->initial_count;
                s->transferred = 0;
            } else d[0] &= ~DMA_EN;
        }
    }
    update_dma_irq(s);
}

static int64_t interval(AnalyzerState *s)
{
    uint64_t ticks = ((uint64_t)s->tim1[PSC] + 1) * ((uint64_t)s->tim1[ARR] + 1);
    return MAX(1, (ticks * 1000000000ULL + 167999999) / 168000000);
}

static void schedule(AnalyzerState *s)
{
    timer_del(s->timer);
    if (s->tim1[CR1] & 1)
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + interval(s));
}

static void timer_tick(void *opaque)
{
    AnalyzerState *s = opaque;
    if (!(s->tim1[CR1] & 1)) return;
    ++s->fixture_ticks;
    if (s->fixture_enable) gpio_input(s, s->gpio[4] + s->fixture_step);
    s->tim1[CNT] = 0;
    s->tim1[SR] |= 1;
    if (s->tim1[DIER] & (1 << 8)) dma_request(s);
    /* TIM1 MMS=update -> TIM8 ITR0 external clock mode 1. */
    if ((s->tim1[CR2] & 0x70) == 0x20 && (s->tim8[CR1] & 1) &&
        (s->tim8[SMCR] & 0x77) == 7) {
        if (++s->tim8[CNT] > s->tim8[ARR]) {
            s->tim8[CNT] = 0;
            s->tim8[SR] |= 1;
            if (s->tim8[CR1] & (1 << 3)) s->tim8[CR1] &= ~1u;
            update_timer_irq(s);
        }
    }
    if (s->tim1[CR1] & (1 << 3)) s->tim1[CR1] &= ~1u;
    schedule(s);
}

static uint64_t analyzer_read(void *opaque, hwaddr addr, unsigned size)
{
    AnalyzerRegion *r = opaque;
    AnalyzerState *s = r->s;
    uint32_t value = 0;
    unsigned reg = addr / 4;
    switch (r->kind) {
    case GPIO: if (reg < ARRAY_SIZE(s->gpio)) value = s->gpio[reg]; break;
    case TIM1: if (reg < ARRAY_SIZE(s->tim1)) value = s->tim1[reg]; break;
    case TIM8: if (reg < ARRAY_SIZE(s->tim8)) value = s->tim8[reg]; break;
    case DMA2: if (reg < ARRAY_SIZE(s->dma)) value = s->dma[reg]; break;
    case FIXTURE:
        switch (reg) {
        case 0: value = 0x534c4131; break; /* SLA1 */
        case 1: value = s->gpio[4]; break;
        case 2: value = s->fixture_step; break;
        case 3: value = s->fixture_enable; break;
        case 4: value = s->fixture_ticks; break;
        case 5: value = s->inject_error; break;
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
        if (reg == 4) return; /* IDR is read-only. */
        if (reg == 6) { s->gpio[5] = (s->gpio[5] | (v & 0xffff)) & ~(v >> 16); return; }
        if (reg < ARRAY_SIZE(s->gpio)) s->gpio[reg] = v;
        break;
    case TIM1:
    case TIM8: {
        uint32_t *t = r->kind == TIM1 ? s->tim1 : s->tim8;
        if (reg >= ARRAY_SIZE(s->tim1)) return;
        if (reg == SR) t[SR] &= v; /* rc_w0 */
        else if (reg == EGR) {
            if (v & 1) {
                t[CNT] = 0;
                if (!(t[CR1] & (1 << 2))) t[SR] |= 1;
            }
        } else t[reg] = (reg == PSC || reg == ARR || reg == CNT) ? v & 0xffff : v;
        if (r->kind == TIM1 && (reg == CR1 || reg == PSC || reg == ARR || reg == EGR)) schedule(s);
        if (r->kind == TIM8) update_timer_irq(s);
        break;
    }
    case DMA2:
        if (reg >= ARRAY_SIZE(s->dma)) return;
        if (reg == 0 || reg == 1) return;
        if (reg == 2 || reg == 3) s->dma[reg - 2] &= ~v; /* IFCR W1C */
        else {
            if (reg == 0x88 / 4 && !(s->dma[reg] & DMA_EN) && (v & DMA_EN)) {
                s->initial_count = stream(s)[1];
                s->transferred = 0;
            }
            s->dma[reg] = v;
        }
        update_dma_irq(s);
        break;
    case FIXTURE:
        switch (reg) {
        case 1: gpio_input(s, v); break;
        case 2: s->fixture_step = v; break;
        case 3: s->fixture_enable = v & 1; break;
        case 4: s->fixture_ticks = 0; break;
        case 5: s->inject_error = v != 0; break;
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
    for (unsigned i = 0; i < 16; ++i) qemu_set_irq(s->pins[i], 0);
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

static void analyzer_finalize(Object *obj) { timer_free(ANALYZER(obj)->timer); }
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
static void analyzer_register(void) { type_register_static(&analyzer_type); }
type_init(analyzer_register)

static void analyzer_machine_init(MachineState *machine)
{
    Clock *sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, 168000000);
    DeviceState *socdev = qdev_new(TYPE_STM32F405_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(socdev));
    qdev_connect_clock_in(socdev, "sysclk", sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(socdev), &error_fatal);
    STM32F405State *soc = STM32F405_SOC(socdev);
    DeviceState *dev = qdev_new(TYPE_ANALYZER);
    object_property_add_child(OBJECT(machine), "analyzer", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    const hwaddr addresses[] = {0x40020c00, 0x40010000, 0x40010400, 0x40026400, 0x60000000};
    for (unsigned i = 0; i < REGION_COUNT; ++i)
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), i, addresses[i], 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(DEVICE(&soc->armv7m), 44));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, qdev_get_gpio_in(DEVICE(&soc->armv7m), 68));
    for (unsigned i = 0; i < 16; ++i)
        qdev_connect_gpio_out(dev, i, qdev_get_gpio_in(DEVICE(&soc->syscfg), 3 * 16 + i));
    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, 0, FLASH_SIZE);
}
static void analyzer_machine_class_init(MachineClass *mc)
{
    mc->desc = "STM32F407 analyzer capture test machine (GPIO/DMA/TIM1/TIM8 subset)";
    mc->init = analyzer_machine_init;
}
DEFINE_MACHINE("stm32f407-sump-analyzer", analyzer_machine_class_init)
