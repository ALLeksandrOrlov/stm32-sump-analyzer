#!/usr/bin/env python3
"""Install the analyzer machine in a supported QEMU source checkout."""

import argparse
from pathlib import Path


SUPPORTED_VERSION = '9.2.4'
MACHINE_SOURCE = Path(__file__).parent / 'overlay/stm32f407-sump-analyzer.c'
MESON_ANCHOR = "hw_arch += {'arm': arm_ss}"
MESON_ENTRY = (
    "arm_ss.add(when: 'CONFIG_STM32F405_SOC', "
    "if_true: files('stm32f407-sump-analyzer.c'))\n"
)

# SYSCFG receives port * 16 + pin. EXTICR and gpio_out must use the pin;
# using the full input index asserts when a port other than GPIOA changes.
SYSCFG_REPLACEMENTS = (
    ('int icrreg = irq / 4;', 'int pin = irq % 16;\n    int icrreg = pin / 4;'),
    ('int startbit = (irq & 3) * 4;', 'int startbit = (pin & 3) * 4;'),
    ('s->gpio_out[irq], level', 's->gpio_out[pin], level'),
)


class OverlayError(ValueError):
    """The source tree does not match a supported overlay installation state."""


def patch_meson(text):
    """Return the build definition with one machine entry, or reject ambiguity."""
    if text.count(MESON_ANCHOR) != 1:
        raise OverlayError('hw/arm/meson.build: expected exactly one ARM source-set anchor')
    entries = text.count(MESON_ENTRY)
    references = text.count('stm32f407-sump-analyzer.c')
    if entries > 1 or references > 1:
        raise OverlayError('hw/arm/meson.build: duplicate analyzer machine entries')
    if entries == 1:
        return text
    if references:
        raise OverlayError('hw/arm/meson.build: unrecognized analyzer machine entry')
    return text.replace(MESON_ANCHOR, MESON_ENTRY + '\n' + MESON_ANCHOR)


def patch_syscfg(text):
    """Accept the supported function either unchanged or fully patched."""
    states = []
    for original, replacement in SYSCFG_REPLACEMENTS:
        counts = (text.count(original), text.count(replacement))
        if counts not in ((1, 0), (0, 1)):
            raise OverlayError(
                f'hw/misc/stm32f4xx_syscfg.c: missing or ambiguous routing anchor: {original}'
            )
        states.append(counts == (0, 1))
    if any(states) and not all(states):
        raise OverlayError('hw/misc/stm32f4xx_syscfg.c: incomplete routing patch')
    if not all(states):
        for original, replacement in SYSCFG_REPLACEMENTS:
            text = text.replace(original, replacement)
    return text


def apply_overlay(source, machine_source=MACHINE_SOURCE):
    """Validate all input files before writing; leave identical files untouched."""
    source = Path(source)
    version = (source / 'VERSION').read_text(encoding='utf-8').strip()
    if version != SUPPORTED_VERSION:
        raise OverlayError(f'expected QEMU {SUPPORTED_VERSION}, found {version!r}')

    meson = source / 'hw/arm/meson.build'
    syscfg = source / 'hw/misc/stm32f4xx_syscfg.c'
    machine = source / 'hw/arm/stm32f407-sump-analyzer.c'
    updates = (
        (meson, patch_meson(meson.read_text(encoding='utf-8')).encode('utf-8')),
        (syscfg, patch_syscfg(syscfg.read_text(encoding='utf-8')).encode('utf-8')),
        (machine, Path(machine_source).read_bytes()),
    )
    changed = [(path, content) for path, content in updates
               if not path.exists() or path.read_bytes() != content]
    for path, content in changed:
        path.write_bytes(content)
    return [path for path, _ in changed]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='QEMU source checkout')
    args = parser.parse_args(argv)
    try:
        apply_overlay(args.source)
    except (OverlayError, OSError, UnicodeError) as error:
        parser.error(str(error))
    print('Installed STM32F407 analyzer machine and SYSCFG port routing fix')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
