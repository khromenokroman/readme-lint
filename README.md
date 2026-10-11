# readme-lint

Сверяет `README.md` C++-проекта с его кодом и выводит расхождения в формате компилятора:

```
README.md:90: ошибка: setItems: в README сигнатура 'as', в интерфейсе 's' [dbus]
```

Работает офлайн: источники правды берутся из самого репозитория (файлы, исходники, `build-deps.lst`,
D-Bus XML), сеть и LLM не нужны. Подходит для запуска в CI.

## Использование

```
readme-lint [ключи] [КАТАЛОГ]
```

| Ключ | Назначение |
|---|---|
| `КАТАЛОГ` | корень проекта, по умолчанию текущий каталог |
| `-f`, `--file ФАЙЛ` | проверяемый README, путь от корня проекта, по умолчанию `README.md` |
| `-V`, `--version` | версия программы |
| `-h`, `--help` | справка |

Код возврата: `0` расхождений нет, `1` найдены расхождения, `2` ошибка запуска.

## Правила

Пока ни одно правило не реализовано, утилита только читает README. План:

| Правило | Что берётся из README | С чем сверяется |
|---|---|---|
| `names` | пути и идентификаторы в `` `...` `` | `git ls-files`, исходники проекта |
| `deps` | пакеты в `apt install ...` | `build-deps.lst` |
| `dbus` | вызовы `busctl call` и `dbus-send` | D-Bus XML интерфейсы проекта |
| `options` | ключи командной строки | `getopt`/`struct option` в коде |
| `snippets` | блоки ` ```cpp ` | компилятор |

## Сборка

```
apt install build-essential cmake libfmt-dev libgtest-dev libgmock-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

DEB-пакет: `cd build && cpack -G DEB`.
