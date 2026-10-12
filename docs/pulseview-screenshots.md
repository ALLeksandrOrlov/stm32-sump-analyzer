# Снимки PulseView и тестовый транспорт

Снимки `docs/images/pulseview-connect.jpg` и `pulseview-i2c.jpg` сделаны
в настоящем PulseView 0.4.2 из Debian Bookworm, с libsigrok 0.5.2.
Устройство подключается через виртуальный serial-порт; имя приходит из SUMP
метаданных текущего `core/sump_protocol.c`. Тестовый стенд использует также
`analyzer_app` и `capture`, заменяя только транспорт и драйвер захвата.

## Воспроизведение

Из корня репозитория:

```sh
docker build -t stm32-sump-analyzer-pulseview:local -f tools/pulseview/Dockerfile .
docker run --rm --name stm32-sump-pulseview \
  -p 127.0.0.1:6080:6080 \
  --mount "type=bind,src=$PWD,dst=/project" \
  stm32-sump-analyzer-pulseview:local
```

Откройте <http://localhost:6080/vnc.html?autoconnect=true&resize=scale>.
Контейнер запускает PulseView с драйвером `ols` и портом
`/tmp/stm32-sump-analyzer`. Для ручного обнаружения выберите
**Openbench Logic Sniffer & SUMP compatibles**, Serial Port и этот путь.
После сканирования появится **STM32 SUMP Analyzer** с восемью каналами.

Для примера I²C используйте 1 МГц, 1 k samples, D0 как SCL и D1 как SDA.
Нажмите Run, затем `D`, найдите I²C и добавьте декодер двойным щелчком.
В настройках декодера назначьте SCL → 0, SDA → 1. Каналы можно подписать SCL/SDA;
клавиши `+` и `Home` позволяют показать первую транзакцию крупнее.
Стенд повторяет каждые 400 отсчётов транзакцию записи по адресу `0x50`
с байтами `0x10`, `0x2A` и ACK. Остальные шесть каналов имеют низкий уровень.
При 1 МГц синтетическая частота SCL равна 100 кГц. Стенд не моделирует
реальное время и не обещает изменение формы сигнала при выборе другой частоты.

Логи и исполняемый файл стенда находятся в `out/pulseview/`.
Остановить окружение: `docker stop stm32-sump-pulseview`.

## Границы проверки

Сигнал синтетический: стенд проверяет обнаружение устройства, формат
метаданных, передачу отсчётов и работу декодера, но не физический USB,
GPIO/DMA STM32 или электрические характеристики платы.

В контейнере установлена отдельная сборка libserialport 0.1.1, фиксированный
коммит `348a6d353af8ac142f68fbf9fe0f4d070448d945`. Для PTY отключён поиск
аппаратных метаданных, а отсутствующие modem-control ioctl трактуются как
неактивные линии только для `/dev/pts/*`. Байты протокола не подменяются.
Патч не входит в прошивку или основной CI-контейнер.

Драйвер OLS автоматически включает флаг FPGA-фильтра при обычных частотах.
Прошивка принимает этот флаг без изменения необработанных отсчётов GPIO.
Проверить декодирование без GUI, когда захват в PulseView остановлен:

```sh
docker exec stm32-sump-pulseview sigrok-cli \
  --driver ols:conn=/tmp/stm32-sump-analyzer --config samplerate=1m \
  --samples 1000 -P i2c:scl=0:sda=1
```

В выводе должны быть `Address write: 50`, `Data write: 10`, `Data write: 2A`, ACK и Stop.
Описание элементов интерфейса: [руководство PulseView 0.4.2](https://sigrok.org/doc/pulseview/0.4.2/manual.html).

VNC слушает loopback внутри контейнера, web-интерфейс опубликован только
на `127.0.0.1` хоста. Стенд предназначен для локальной разработки.
