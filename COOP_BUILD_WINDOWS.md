# Сборка CBN-Coop на своём компьютере (Windows)

Те же настройки, что у сборки в GitHub Actions (workflow `matrix`, Windows MSVC):
Visual Studio 2022, vcpkg, Ninja, Release, tiles + звук, статическая линковка.
Результат — готовая папка с игрой, как в архиве из Actions.

## 1. Один раз: установка инструментов

Нужно примерно 30 ГБ свободного места на диске NTFS.

1. **Git for Windows** — https://git-scm.com/download/win (настройки по умолчанию).
2. **Visual Studio 2022 Community** — https://visualstudio.microsoft.com/
   (или «Build Tools for Visual Studio 2022», если IDE не нужна).
   В установщике отметить нагрузку **«Разработка классических приложений на C++»**
   (Desktop development with C++). В ней по умолчанию уже есть MSVC x64,
   Windows SDK и **«Средства CMake C++ для Windows»** (CMake и Ninja) — их не снимать.
3. **vcpkg** — в обычной командной строке (`cmd`):

   ```cmd
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   setx VCPKG_ROOT C:\vcpkg
   ```

   Клонировать именно полностью (без `--depth`): vcpkg нужна история для версии
   библиотек из `msvc-full-features/vcpkg.json`. Путь без пробелов.
   После `setx` закройте и заново откройте окна консоли.

## 2. Один раз: получить исходники

Путь не должен содержать пробелов и кириллицы, лучше короткий:

```cmd
mkdir C:\dev
cd C:\dev
git clone https://github.com/tsvetkovdm/CBN-Coop.git
```

Полная история весит несколько гигабайт; если она не нужна, добавьте `--depth 50`.

## 3. Сборка

Дважды щёлкнуть `C:\dev\CBN-Coop\coop-build.bat` или из консоли:

```cmd
cd C:\dev\CBN-Coop
coop-build.bat
```

Скрипт сам найдёт Visual Studio и vcpkg, включит x64-окружение компилятора,
соберёт игру и разложит её в **`C:\dev\CBN-Coop\out\game`**
(`cataclysm-bn-tiles.exe` + `data`, `gfx`, `mods` и т. д.).

Сколько ждать:

- первый раз — долго: vcpkg скачивает и собирает SDL3, sqlite, zlib и пр.
  (обычно 20–40 минут), потом сама игра (ещё 20–60 минут, зависит от процессора);
- дальше — только изменённые файлы: правка в `src/coop/` пересобирается за 1–3 минуты.

Параметры (можно передавать и в `.bat`):

| Команда | Что делает |
|---|---|
| `coop-build.bat` | обычная (инкрементальная) сборка |
| `coop-build.bat -Reconfigure` | заново запустить настройку CMake (после смены `CMakeLists.txt`, `vcpkg.json`) |
| `coop-build.bat -Clean` | удалить папку сборки и собрать с нуля |
| `coop-build.bat -Out D:\games\cbn` | положить игру в другую папку |
| `coop-build.bat -Jobs 4` | ограничить число потоков (если не хватает памяти) |

## 4. Обновиться до свежего main и пересобрать

```cmd
cd C:\dev\CBN-Coop
git pull
coop-build.bat
```

Сохранения и настройки в `out\game` при пересборке не удаляются
(скрипт только копирует поверх файлы игры).

## 5. Две копии на одном компьютере (хост и клиент)

Обе копии запускаются из одной папки, но с разными папками пользователя,
чтобы миры, персонажи и настройки не смешивались:

```cmd
cd C:\dev\CBN-Coop\out\game
start cataclysm-bn-tiles.exe --userdir host\ --coop-host
start cataclysm-bn-tiles.exe --userdir client\ --coop-join 127.0.0.1
```

Сначала хост заходит в игру, потом клиент подключается (см. `COOP.md`).
Чтобы отдать сборку другу, заархивируйте `out\game` без папок `host`, `client`
и других папок с сохранениями.

## Если что-то пошло не так

- **`vcpkg not found`** — не выполнен шаг 1.3 или не перезапущена консоль после `setx`.
- **`Visual Studio 2022 ... not found`** — в установщике VS не отмечена нагрузка C++.
- **`cmake` / `ninja` not found** — в установщике VS («Изменить» → «Отдельные компоненты»)
  добавить «Средства CMake C++ для Windows».
- **Ошибка vcpkg про `builtin-baseline`** — vcpkg склонирован с `--depth`:
  выполнить `git -C C:\vcpkg fetch --unshallow` или клонировать заново.
- **Ошибка при скачивании (gettext, mesa)** — настройка CMake качает пару архивов
  с GitHub; проверьте интернет/VPN и запустите `coop-build.bat -Reconfigure`.
- **Компилятор падает с нехваткой памяти** — `coop-build.bat -Jobs 4` (или 2).
- **Странные ошибки после `git pull`** — сначала `-Reconfigure`, если не помогло — `-Clean`.

Если ошибка не уходит, пришлите первые строки, где встречается `error`
(не последние: последние обычно только «build stopped»).

## Для разработки в Visual Studio (необязательно)

Можно открыть папку репозитория в VS 2022 («Открыть локальную папку»), выбрать
пресет `windows-tiles-sounds-x64-msvc` и собирать/отлаживать из IDE —
подробности в `docs/en/dev/guides/building/vs_cmake.md`. Для обычной сборки
«чтобы поиграть» достаточно `coop-build.bat`.
