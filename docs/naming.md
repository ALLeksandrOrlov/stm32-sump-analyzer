# Имена проекта и модулей

| Сущность | Имя |
| --- | --- |
| Репозиторий | `stm32-sump-analyzer` |
| Устройство в PulseView и USB Product | `STM32 SUMP Analyzer` |
| USB Manufacturer | `stm32-sump-analyzer` |
| USB Configuration / Interface | `SUMP CDC` |
| Проект CubeMX | `stm32_sump_analyzer.ioc` |
| ELF, HEX, BIN, MAP | `stm32_sump_analyzer.*` |
| Контейнер CI | `stm32-sump-analyzer-ci` |
| Контейнер интерфейса PulseView | `stm32-sump-analyzer-pulseview` |
| Машина QEMU | `stm32f407-sump-analyzer` |
| Исполняемый файл расширенного QEMU | `qemu-system-arm-sump-analyzer` |

Публичные строки устройства и версия прошивки определены в
`core/analyzer_identity.h`. SUMP и адаптер USB используют один заголовок.
Версия разработки — `0.1.0-dev`; перед выпуском её обновляют вместе с CHANGELOG.
Сигнатура SUMP `1ALS`, коды команд, значения метаданных и USB VID/PID сохранены:
они участвуют в совместимости с драйверами. `NoFPGA` означает отсутствие FPGA,
а не название устройства. Имена ST HAL/CMSIS и генерируемых IRQ не переименовываются.

Модули названы по ответственности: `sump_protocol` разбирает протокол,
`capture` управляет захватом, `analyzer_app` связывает команды и транспорт,
`capture_stm32` обслуживает периферию, `analyzer_board` подключает приложение к плате.
Эти имена не зависят от отображаемого названия устройства.

Величины с единицами получают явные суффиксы: `timer_clock_hz`,
`CAPTURE_MAX_SAMPLE_RATE_HZ`, `CAPTURE_SUMP_CLOCK_HZ`.
`CAPTURE_BUFFER_SAMPLES` измеряется в отсчётах; сейчас каждый занимает один байт.
`samples_sent` считает принятые транспортом отсчёты, `tx_in_flight` отмечает
незавершённую передачу, `tx_contains_samples` отличает захват от служебного ответа.
Обработчики `capture_stm32_tim8_update_irq` и `capture_stm32_dma2_stream5_irq`
указывают конкретный источник прерывания.

Старые бинарники и неиспользуемый генерируемый Makefile удалены. `GNUmakefile`
остаётся единственной поддерживаемой сборкой; при повторной генерации CubeMX
его не перезаписывает. Правила именования не требуют менять пользовательское
имя локального каталога, Docker context или уже установленной VM Lima.
