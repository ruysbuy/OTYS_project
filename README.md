# minidocker: Легковесный аналог Docker на C++

Проектная работа в рамках курса OTUS. Представляет собой минималистичную систему контейнеризации, написанную с нуля на C++20.

## 1. Описание проекта

Проект реализует базовый функционал, аналогичный Docker:

- Изоляция процессов на уровне ядра Linux (namespaces).
- Ограничение доступа к файловой системе (`pivot_root` + OverlayFS).
- Организация интерактивного ввода-вывода через псевдотерминал (PTY).
- Управляющий демон, работающий в фоне и администрирующий несколько контейнеров.
- Полный жизненный цикл: запуск (включая фоновый), сбор логов, безопасная остановка, подключение к терминалу (attach).
- Поддержка Rootless-режима (архитектурно) и Rootful.

## 2. Требования и сборка

**Требования к системе:**

- Linux (тестировалось на Ubuntu / Xilinx)
- Компилятор с поддержкой **C++20** (`g++ >= 12`)
- **CMake** (>= 3.20)
- Системные утилиты: `libutil-dev` (для PTY), `curl`, `tar`

**Инструкция по сборке:**

```bash
# 1. Установка зависимостей
sudo apt update
sudo apt install -y build-essential cmake libutil-dev curl tar

# 2. Скачивание тестовой rootfs (Alpine Linux)
chmod +x scripts/download-rootfs.sh
./scripts/download-rootfs.sh

# 3. Сборка проекта
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## 3. Архитектура решения

Проект разделен на три ключевых компонента:

- **Клиент (`minidocker`)**: CLI-утилита. Отвечает за парсинг аргументов, отправку JSON-команд демону по Unix-сокету (`/tmp/minidocker.sock`) и трансляцию ввода-вывода (в случае PTY).
- **Демон (`minidockerd`)**: Работает в фоне. Принимает соединения, управляет жизненным циклом контейнеров, хранит метаданные и логи в `/root/.local/share/minidocker/`.
- **Контейнер**: Создаётся демоном через системный вызов `clone()`.

### Изоляция (Namespaces)

Для каждого контейнера создаётся набор изолированных пространств имён:

```
CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET
```

- **NEWPID**: Главный процесс контейнера получает PID 1.
- **NEWNS**: Изоляция точек монтирования.
- **NEWNET**: Создаётся свой сетевой стек (в коде автоматически поднимается интерфейс loopback).

### Файловая система

Используется **OverlayFS**: `lowerdir` (неизменяемая rootfs Alpine), `upperdir` (изменения контейнера) и `workdir`. После сборки слоёв вызывается `pivot_root` для смены корневого каталога.

## 4. Интересные принятые решения (Архитектурные фишки)

- **Attach-протокол**: В отличие от `docker exec`, который запускает новый процесс, наш `attach` подключается к существующему псевдотерминалу (PTY) главного процесса контейнера. Демон хранит master-дескриптор PTY. При вызове `attach` клиентский сокет добавляется в `epoll`-цикл демона, и байты начинают летать напрямую между терминалом пользователя и контейнером. Отсоединение клиента не убивает контейнер.
- **Мониторинг завершения**: После запуска контейнера демон порождает легковесный поток, который вызывает `waitpid()`. Как только процесс умирает, статус в файле `state.json` мгновенно меняется на `stopped`, и записывается `exit_code`.
- **Безопасная остановка**: Команда `stop` отправляет процессу `SIGTERM` (ожидание 5 секунд), и только если процесс не завершился (что часто бывает с PID 1) — `SIGKILL`.

## 5. Разбор возникших проблем и их решение (Опыт разработки)

В ходе написания и запуска проекта в среде Ubuntu были выявлены и устранены следующие системные и архитектурные проблемы:

### Проблема с монтированием `/proc` (`Operation not permitted`)

- **Причина**: Внутри User Namespace (`CLONE_NEWUSER`) ядро требует явного указания флагов безопасности при монтировании системных ФС.
- **Решение**: В `container.cpp` добавлены флаги `MS_NOSUID | MS_NODEV | MS_NOEXEC`.

### Проблема с `uid_map` / `gid_map`

- **Причина**: Ребёнок пытался сам записать свой UID в `/proc/self/uid_map`, но в новом namespace ещё нет маппинга.
- **Решение**: Родительский процесс (демон) теперь ожидает создания namespace и сам записывает `/proc/<child_pid>/uid_map`.

### Зависание `sh -c` в неинтерактивном режиме

- **Причина**: Без флага `-t` оболочка `sh` ждала ввода от `stdin`, который не был перенаправлен.
- **Решение**: Для неинтерактивных контейнеров `stdin` теперь перенаправляется в `/dev/null`.

### Двойное чтение логов (потеря вывода)

- **Причина**: При запуске в обычном режиме демон запускал фоновый поток чтения логов и одновременно пытался вывести их клиенту.
- **Решение**: Фоновый поток чтения логов (`log_thread`) теперь запускается только если контейнер запущен с флагом `-d` (detached).

### Проблема с короткими флагами (например, `-td`)

- **Причина**: Парсер CLI не понимал слитные флаги.
- **Решение**: В `main.cpp` добавлен цикл, разбирающий каждый символ в аргументе (поддержка `-td`, `-it` и т.д.).

## 6. CLI Команды и флаги

| Команда | Флаги | Описание |
|---|---|---|
| `run` | `--rootfs DIR` (обязательно), `-d` / `--detach`, `-t` / `--tty`, `-i` / `--interactive`, `--memory MB`, `--cpus N` | Создать и запустить контейнер. |
| `ps` | `-a` / `--all` | Показать список контейнеров (по умолчанию только `running`). |
| `logs` | `-f` / `--follow` | Вывести логи контейнера (`-f` — стриминг в реальном времени). |
| `stop` | — | Остановить работающий контейнер по ID. |
| `attach` | — | Подключиться к PTY работающего контейнера (требует запуска с `-t`). |

Глобальный флаг `--socket PATH` позволяет указать путь к сокету демона.

## 7. Примеры работы (Логи выполненных проверок)

### 1. Проверка базового запуска и изоляции (PID 1)

```bash
$ sudo ./build/minidocker --socket /tmp/minidocker.sock run --rootfs ./rootfs/alpine -- /bin/sh -c 'echo "My PID is $$"'
My PID is 1
```

### 2. Проверка интерактивного режима (PTY)

```bash
$ sudo ./build/minidocker --socket /tmp/minidocker.sock run -t --rootfs ./rootfs/alpine -- /bin/sh
/ # ls /
bin    etc    lib    mnt    proc   run    srv    tmp    var
dev    home   media  opt    root   sbin   sys    usr
/ # cat /proc/1/comm
sh
/ # exit
```

### 3. Проверка фонового запуска и логов

```bash
$ sudo ./build/minidocker --socket /tmp/minidocker.sock run -d --rootfs ./rootfs/alpine -- /bin/sh -c 'echo "Log line 1"; echo "Log line 2"; sleep 300'
3cdd35f4f387

$ sudo ./build/minidocker --socket /tmp/minidocker.sock ps
ID             PID      STATUS     COMMAND
3cdd35f4f387   5752     running    /bin/sh

$ sudo ./build/minidocker --socket /tmp/minidocker.sock logs 3cdd35f4f387
Log line 1
Log line 2
```

### 4. Проверка безопасной остановки

```bash
$ sudo ./build/minidocker --socket /tmp/minidocker.sock stop 3cdd35f4f387

$ sudo ./build/minidocker --socket /tmp/minidocker.sock ps -a
ID             PID      STATUS     COMMAND
3cdd35f4f387   5752     stopped    /bin/sh
```

### 5. Прохождение автоматизированных тестов

```bash
$ sudo ./scripts/run-tests.sh
Internal ctest changing into directory: /home/xilinx/Desktop/otus/project/build
Test project /home/xilinx/Desktop/otus/project/build
    Start 1: integration_container
1/2 Test #1: integration_container ............   Passed    6.60 sec
    Start 2: daemon_protocol
2/2 Test #2: daemon_protocol ..................   Passed    4.51 sec

100% tests passed, 0 tests failed out of 2
```

## 8. Ограничения текущей реализации

- Нет поддержки скачивания образов по сети (`pull`). Rootfs готовится скриптом.
- Нет проброса портов (port publishing), сеть изолирована (доступен только loopback).
- Нет интеграции с `seccomp` / `AppArmor` профилями.