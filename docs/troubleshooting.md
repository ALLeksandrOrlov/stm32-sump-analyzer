# Диагностика

## Docker недоступен

```sh
docker version
docker context ls
```

В выводе `docker version` должны присутствовать Client и Server. На Linux запустите
Docker Engine; на macOS — Docker Desktop или используемую Linux VM. Ошибка
`Cannot connect to the Docker daemon` возникает до сборки и не связана с прошивкой.

Проверьте также доступность Buildx:

```sh
docker buildx version
```

На Linux достаточно Docker Engine с плагином Buildx. На macOS можно использовать
Docker Desktop либо Docker Engine внутри Lima. Linux VM должна иметь доступ
на запись к каталогу репозитория; для сборки QEMU удобно выделить 4 CPU,
4 ГиБ памяти и около 20 ГиБ диска под образы и кеш.

Если ваша VM и Docker context называются `sump-docker`, команды управления:

```sh
limactl start sump-docker
docker context use sump-docker
limactl stop sump-docker
```

Последняя команда останавливает VM и сохраняет образы. При другом имени VM
подставьте его; проект не требует конкретного имени или способа установки Docker.
После переноса репозитория проверьте разрешённые каталоги Docker Desktop или mounts Lima.

## Ошибка сборки образа

```sh
docker buildx build --progress=plain --target ci --load -t stm32-sump-analyzer-ci:local .
```

Смотрите первую ошибку, а не только финальное `failed to solve`.
Проверьте доступ к Docker Hub, Debian, PyPI, GitHub и GitLab: сборка получает
базовый образ, пакеты и закреплённые зависимости QEMU. При изменении toolchain
обновляйте Dockerfile, затем повторяйте полный сценарий. Не обходите провал тестов
для получения release artifact.

## Ошибка тестов

Смотрите `out/ci/reports/summary.md`, затем одноимённый `.log`. В GitHub Actions
скачайте artifact `test-reports-<commit>`. Проверки выполняются и после первого
провала, поэтому можно увидеть несколько независимых ошибок за один запуск.

- `cubemx`: проверьте изменения вне USER CODE и парность маркеров. После
  намеренной регенерации сначала просмотрите diff, потом обновляйте эталон.
- `native` / `sanitizers`: assert, выход за память, UB или ошибка контракта USB.
- `coverage`: сборка с GCC/gcov, выполнение нативных тестов, формирование отчёта
  и пороги 95% строк / 90% ветвей. При падении покрытия найдите непроверенный
  контракт в HTML-отчёте; не снижайте порог только для успешной сборки.
- `qemu-core`: загрузка тестового ELF, semihosting или поведение переносимого ядра.
- `qemu-stm32`: периферия, DMA, EXTI, IRQ или модель анализатора.
- `firmware`: компилятор, linker, память или интеграция STM32/USB.

`unsupported machine type stm32f407-sump-analyzer` означает запуск обычного QEMU.
Используйте контейнер либо `QEMU=qemu-system-arm-sump-analyzer` внутри него.
`QEMU failed: ... timed out` — тест завис или слишком долго выполняется;
проверьте, достигается ли Reset_Handler и не попал ли процессор в fault handler.

## Файлы не создаются на macOS

Docker daemon внутри VM должен видеть bind-mount каталога проекта. При использовании
Docker Desktop разрешите доступ к каталогу, при Lima проверьте mounts. Сценарий
запускает контейнер с UID/GID текущего пользователя, чтобы результаты не становились
root-owned. Не используйте `sudo make docker-build` для обхода ошибки доступа.

## Release не появился

1. Убедитесь, что отправлен тег вида `v0.1.0`, а не только commit или ветка.
2. Проверьте полный успех job `Docker / native / QEMU / firmware`.
3. Проверьте лог release job: SHA256, commit, формат тега, права `contents: write`.
4. Проверьте, не существует ли Release с этим тегом: pipeline не перезаписывает его.

Ручной workflow_dispatch и PR собирают артефакты, но не публикуют Release.
GitHub Actions использует файлы отправленного commit; локальные изменения
не участвуют в удалённой сборке.
