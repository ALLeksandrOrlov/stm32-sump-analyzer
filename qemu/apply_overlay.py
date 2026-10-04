#!/usr/bin/env python3
"""Install the analyzer machine into a clean QEMU v9.2.4 source checkout."""
import argparse
from pathlib import Path
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
args = parser.parse_args()
root = args.source
if (root / 'VERSION').read_text().strip() != '9.2.4':
    raise SystemExit('This overlay is tested with QEMU 9.2.4 only')
shutil.copyfile(Path(__file__).parent / 'overlay/stm32f407-sump-analyzer.c',
                root / 'hw/arm/stm32f407-sump-analyzer.c')
p = root / 'hw/arm/meson.build'
line = "arm_ss.add(when: 'CONFIG_STM32F405_SOC', if_true: files('stm32f407-sump-analyzer.c'))\n"
text = p.read_text()
if line not in text:
    p.write_text(text.replace("hw_arch += {'arm': arm_ss}", line + "\nhw_arch += {'arm': arm_ss}"))
# Upstream 9.2.4 indexes EXTICR/gpio_out with the full port+pin number,
# which asserts for any GPIO port other than A. Route by pin, select by port.
p = root / 'hw/misc/stm32f4xx_syscfg.c'
text = p.read_text()
text = text.replace('int icrreg = irq / 4;', 'int pin = irq % 16;\n    int icrreg = pin / 4;')
text = text.replace('int startbit = (irq & 3) * 4;', 'int startbit = (pin & 3) * 4;')
text = text.replace('s->gpio_out[irq], level', 's->gpio_out[pin], level')
p.write_text(text)
print('Installed STM32F407 analyzer machine and SYSCFG port routing fix')
