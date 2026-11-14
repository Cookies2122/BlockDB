<h1 align=center><code>BlockDB</code></h1>

<div align=center>
  <img src=https://img.shields.io/badge/Counter--Strike%202-Metamod%202.0%20(KHook)-orange?style=flat-square alt=metamod>
  <a href=https://github.com/Cookies2122/BlockDB/commits><img src=https://img.shields.io/github/last-commit/Cookies2122/BlockDB?style=flat-square alt="last commit"></a>
  <a href=LICENSE><img src=https://img.shields.io/github/license/Cookies2122/BlockDB?style=flat-square alt=license></a>
</div>
<br>

Общая база читеров для проектов CS2. Когда админ банит игрока через Admin System, бан уходит в BlockDB. Когда игрок заходит, плагин проверяет его по базе: если ваш проект блокирует таких игроков, его кикает, а если у него есть баны на других проектах, админы видят это в чате.

> Версия 1.1.1. Работает на Metamod 2.0 (KHook) и новом SDK после обновления CS2 от 12.11.2026.

## Что нужно

- [Metamod:Source](https://github.com/alliedmodders/metamod-source) 2.0 (KHook)
- [Utils](https://github.com/Pisex/cs2-menus)
- [Admin System](https://github.com/Pisex/cs2-admin_system)

Плагин обращается к `api.blockdb.net`: проверяет игроков при входе и отправляет туда баны и разбаны с вашего сервера. Ключ сервера после `db_pair` хранится в `addons/data/blockdb_data.ini`, не выкладывайте этот файл.

В папке `counterstrikesharp/IksAdmin_BlockDB` лежит версия для CounterStrikeSharp и Iks Admin.

## Установка

Соберите плагин (раздел [Сборка](#сборка)) и скопируйте содержимое `build/package` в папку `csgo` на сервере. В пакете лежит:

| Файл | Куда |
| --- | --- |
| `blockdb.so` | `addons/admin_modules/` |
| `blockdb.vdf` | `addons/metamod/` |

Эти файлы в пакет не входят, положите их из репозитория сами:

| Файл | Куда |
| --- | --- |
| `translations/blockdb.phrases.txt` | `addons/translations/` |

После этого перезапустите сервер или смените карту.

## Команды

| Команда | Что делает |
| --- | --- |
| `db_pair <код>` | привязать сервер к BlockDB (консоль сервера) |

## Переводы

Файл `addons/translations/blockdb.phrases.txt`, языки: ru, en, ua. Его можно править под себя.

<details>
<summary>Все фразы</summary>

```
"Phrases"
{
    "Notify_Player"
    {
        "en"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Player: {LIGHTBLUE}%s"
        "ru"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Игрок: {LIGHTBLUE}%s"
        "ua"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Гравець: {LIGHTBLUE}%s"
    }
    "Notify_BanCount"
    {
        "en"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Active bans: {RED}%d"
        "ru"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Активных банов: {RED}%d"
        "ua"   "{DEFAULT}[{BLUE}BlockDB{DEFAULT}] Активних банів: {RED}%d"
    }
}
```

</details>

## Что изменено

В этой версии:

- Переведено на новый SDK ([Wend4r/sourcesdk](https://github.com/Wend4r/sourcesdk)), старый после обновления CS2 от 12.11.2026 уже не подходит.
- Работает только на Metamod 2.0 (KHook), старый Metamod на SourceHook больше не поддерживается.
- Заголовки Utils обновлены под текущую версию, имена интерфейсов теперь `UTILS_INTERFACE` и `MENUS_INTERFACE`.
- Хуки переписаны на KHook из Metamod 2.0, без прослоек под старый SourceHook.

## Сборка

Собирается на Linux. Нужны clang, python3, cmake и [AMBuild](https://github.com/alliedmodders/ambuild). Все команды ниже выполняются из одной папки.

**1. SDK.** Нужен [sourcesdk](https://github.com/Wend4r/sourcesdk) не старее обновления CS2 от 12.11.2026. Собирается один раз:

```bash
git clone --recursive https://github.com/Wend4r/sourcesdk hl2sdk-cs2
cmake -S hl2sdk-cs2 -B hl2sdk-cs2/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSOURCESDK_AM_DEFINES=ON -DSOURCESDK_USE_MINIMAL_PROTOS=OFF
cmake --build hl2sdk-cs2/build -j$(nproc)
export HL2SDKCS2=$PWD/hl2sdk-cs2
```

**2. Metamod.** Нужен Metamod 2.0 с KHook, ветка `master`. Со старым Metamod на SourceHook сборка остановится с ошибкой.

```bash
git clone --recursive https://github.com/alliedmodders/metamod-source
```

**3. Плагин.** Папка SchemaEntity должна лежать рядом с папкой плагина.

```bash
git clone https://github.com/{o}/SchemaEntity
git clone https://github.com/Cookies2122/BlockDB
cd BlockDB
mkdir build && cd build
python3 ../configure.py --enable-optimize --sdks=cs2 --targets=x86_64 --mms_path=$PWD/../../metamod-source --hl2sdk-manifests=../hl2sdk-manifests
ambuild
```

Готовые файлы появятся в `build/package`.

## Автор

Плагин написал [\_ded\_cookies](https://github.com/Cookies2122).

## Лицензия

GPL-3.0, текст в файле [LICENSE](LICENSE).
