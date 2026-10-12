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
Версия разработки — `0.1.0-dev`; перед выпуском её обновляют в этом заголовке.
Сигнатура `1ALS` и коды команд определены протоколом SUMP. USB VID/PID находятся
в конфигурации дескрипторов USB. Метаданные содержат имя устройства и версию
прошивки. Имена ST HAL/CMSIS и генерируемых IRQ определяются их публичными API.

Модули названы по ответственности: `sump_protocol` разбирает протокол,
`capture` управляет захватом, `analyzer_app` связывает команды и транспорт,
`capture_stm32` обслуживает периферию, `analyzer_board` подключает приложение к плате.
Эти имена не зависят от отображаемого названия устройства.

Величины с единицами получают явные суффиксы: `timer_clock_hz`,
`sample_period_ns`, `CAPTURE_MAX_SAMPLE_RATE_HZ`.
`CAPTURE_BUFFER_SAMPLES` измеряется в отсчётах; сейчас каждый занимает один байт.
`sump_flags` относится только к формату SUMP и хранится в приложении. Конфигурация
контроллера содержит физические параметры захвата без флагов протокола.
`samples_sent` считает принятые транспортом отсчёты, `tx_in_flight` отмечает
незавершённую передачу, `tx_contains_samples` отличает захват от служебного ответа.
Обработчики `capture_stm32_tim8_update_irq` и `capture_stm32_dma2_stream5_irq`
указывают конкретный источник прерывания.

`GNUmakefile` — единственная поддерживаемая точка входа сборки; при повторной
генерации CubeMX его не перезаписывает. Имя локального каталога и Docker context
пользователь выбирает независимо от имени устройства.
