# Расширение QEMU для STM32F407-анализатора

`overlay/stm32f407-sump-analyzer.c` добавляет отдельную машину `stm32f407-sump-analyzer`
на базе STM32F405/Cortex-M4. Это целевая модель периферии, которую использует
прошивка, а не полная эмуляция STM32F407. Обычная `netduinoplus2` сохраняется.

## Тестовые окружения

| Команда | Машина | Что исполняется |
| --- | --- | --- |
| `make qemu-test` | `netduinoplus2` | Тесты независимого C11-ядра на Cortex-M4 |
| `make qemu-stm32-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | Драйвер захвата и модель нужных регистров STM32 |
| `make qemu-rtos-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | FreeRTOS ARM_CM4F, задачи приложения и USB TX, MessageBuffer и управляемые тестовые порты; также запускает следующую проверку |
| `make qemu-rtos-capture-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | Те же задачи с настоящим драйвером захвата и переходником отложенного IRQ |

Тест RTOS использует те же исходники ядра и runtime, что и прошивка.
Дополнительная задача управляет тестовыми портами USB и захвата: создаёт
прерывания, задерживает завершение IN, временно отклоняет отправку и меняет
подключение. Проверяются заполнение очередей, NAK, SUMP RESET, переподключение,
ошибка RX при активной передаче и неизменность заимствованного USB-буфера.
Ожидающий до старта планировщика USB IRQ разрешается через `activate()` уже
в контексте задачи. Тестовая группировка NVIC соответствует настройке HAL.

Второй ELF RTOS связывает настоящий `capture_stm32` и `capture_task_events`
с задачами: DMA пишет отсчёты, ISR приоритета 1 выставляет отложенный IRQ
приоритета 5, задача получает событие и отправляет результат через MessageBuffer.
Оба ELF исполняют планировщик и используют подмену USB. Совместная работа
с контроллером OTG проверяется на [реальной плате](../docs/hardware-validation.md).

## Что моделируется

| Узел | Реализованный объём |
| --- | --- |
| GPIOD | Регистры конфигурации, IDR, ODR, BSRR; изменение входов и EXTI |
| SYSCFG | Используется штатная модель; исправлена адресация порт/номер вывода |
| TIM1 | PSC/ARR, CEN, update, TRGO, запрос DMA; фиксированный clock 168 МГц |
| TIM8 | ITR0 от TIM1, счётчик посттриггерных отсчётов, OPM, update IRQ |
| DMA2 | Stream 5/channel 6, байтовый peripheral-to-memory, MINC, CIRC, NDTR, TC/TE IRQ |
| Тестовая плата | Управляемый входной сигнал и инъекция ошибки DMA |

DMA действительно пишет в эмулируемую RAM, меняет NDTR и формирует прерывания
NVIC; тест запускает **тот же** `platform/stm32/capture_stm32.c`, CMSIS и HAL
GPIO/NVIC, что и прошивка. USB заменён транспортом в памяти. PLL/RCC startup
не запускается; определение частоты заменено фиксированным значением.

Регистры тестового генератора по адресу `0x60000000` не существуют в настоящем
STM32. Они используются только интеграционным тестом:

| Смещение | Назначение |
| --- | --- |
| 0x00 | Read-only ID `0x534c4131` (`SLA1`) |
| 0x04 | Уровни GPIOD IDR, чтение/запись |
| 0x08 | Шаг счётчика входного сигнала |
| 0x0c | Автоизменение входа перед каждым отсчётом TIM1, bit 0 |
| 0x10 | Число событий TIM1, запись сбрасывает счётчик |
| 0x14 | Инъекция ошибки на следующем DMA-запросе |

## Сборка

Весь путь включён в корневой Dockerfile и `make docker-build`.
Для отдельной сборки на Linux нужны C/C++ compiler, Ninja, Python с venv,
pkg-config, bzip2, GLib >= 2.66, libfdt и zlib с development headers:

```sh
git clone --depth 1 --branch v9.2.4 https://github.com/qemu/qemu.git /tmp/qemu-analyzer
# Ожидаемый commit: b2995afed254e4804611df52210067a4d5dbdf0b
python3 qemu/apply_overlay.py /tmp/qemu-analyzer
mkdir -p out/qemu-build
cd out/qemu-build
/tmp/qemu-analyzer/configure --target-list=arm-softmmu --disable-docs \
  --disable-tools --disable-user --disable-plugins --disable-pixman
ninja qemu-system-arm
cd ../..
make qemu-stm32-test QEMU="$PWD/out/qemu-build/qemu-system-arm"
make qemu-rtos-test QEMU="$PWD/out/qemu-build/qemu-system-arm"
```

Установщик оверлея проверяет версию и может запускаться повторно. Он добавляет
файл машины, строку в `hw/arm/meson.build` и исправляет маршрутизацию в
`hw/misc/stm32f4xx_syscfg.c`. В QEMU 9.2.4 исходная адресация EXTICR использует
полный номер port*16+pin вместо pin; на GPIOD это приводит к assert.

## Границы проверки

Не реализованы USB OTG, PLL startup, другие потоки/каналы DMA, FIFO, half-transfer
IRQ, DBM, memory-to-memory, произвольные режимы таймеров, PWM, input capture,
другие GPIO-порты, миграция состояния и точная модель шин. Регистры, необходимые
инициализации HAL, сохраняют значения; не все их аппаратные побочные эффекты
моделируются. PSC/ARR меняют интервал обновлений, но непрерывное чтение фазы CNT
TIM1 не поддержано. Неподдерживаемые направление, ширина, канал DMA или
включённый PINC вызывают TE.
GPIO IDR читается байтами или словами; запись регистров — 32-битная.

Модель предназначена для поиска программных ошибок в конфигурации, состоянии
DMA, обработчиках и порядке данных. Она не измеряет пропускную способность,
латентность ISR на реальном STM32, джиттер триггера или электрические свойства.
Интеграционные тесты также не проверяют полноценную USB-прошивку с PulseView.

Штатная поддержка STM32 и её ограничения описаны в
[документации QEMU](https://www.qemu.org/docs/master/system/arm/stm32.html).
Описание регистров — [ST RM0090](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-407-415-417-437-455-469-application-note-stmicroelectronics.pdf).

Файл модели распространяется под GPL-2.0-or-later для включения в QEMU.
