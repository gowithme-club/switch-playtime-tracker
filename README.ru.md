<p align="center">
  <a href="https://gowithme.club"><img src=".github/assets/banner.png" alt="Трекер игрового времени для Switch от gowithme.club: сессии и часы со Switch с Atmosphere" width="100%"></a>
</p>

<p align="center">
  <a href="README.md">English</a> · <b>Русский</b>
</p>

<p align="center">
  <a href="https://github.com/gowithme-club/switch-playtime-tracker/releases/latest"><img alt="Последний релиз" src="https://img.shields.io/github/v/release/gowithme-club/switch-playtime-tracker?style=flat-square&labelColor=131C16&color=C7F547&label=release"></a>
  <img alt="Switch с Atmosphere" src="https://img.shields.io/badge/Switch-Atmosphere-C7F547?style=flat-square&labelColor=131C16">
  <a href="LICENSE"><img alt="Лицензия MIT" src="https://img.shields.io/badge/license-MIT-C7F547?style=flat-square&labelColor=131C16"></a>
  <a href="https://gowithme.club"><img alt="gowithme.club" src="https://img.shields.io/badge/profile-gowithme.club-C7F547?style=flat-square&labelColor=131C16"></a>
</p>

# Трекер игрового времени для Switch

Sysmodule Atmosphere для прошитой Nintendo Switch: записывает, во что и сколько
вы играете, и отправляет это в ваш профиль на
[gowithme.club](https://gowithme.club) — кросс-платформенном трекере игровой
статистики. Стартует вместе с системой и работает в фоне: без Nintendo Switch
Online, без трафика к Nintendo и без компьютера рядом во время игры.

Исходный код открыт, чтобы было видно, что именно работает на вашей консоли и
что с неё уходит.

## Что вы получите

- **Время в каждой игре**: каждая сессия с началом, концом и длительностью — из
  журнала активности самой консоли.
- **Все профили консоли**: каждый пользователь подключается отдельно, поэтому
  на общей Switch статистика у каждого своя.
- **«Сейчас играет»**: друзья видят, во что вы играете, а при закрытии игры
  всплывёт уведомление, сколько вы играли.
- **Без сети тоже**: дни без интернета догоняются из журнала, когда консоль
  снова выйдет в сеть.
- **Один профиль на все консоли**: Switch, PS5, PS3, Steam, Xbox и PSN в одном
  месте на gowithme.club.

## Как это устроено

```text
Switch ── sysmodule читает системный журнал активности (pdm)
            │
            └── HTTP ──► gowithme.club ──► ваш профиль, друзья, отчёты в Telegram
```

Модуль только сообщает сырые события. Названия игр, обложки, итоги и устранение
дублей — на сервере gowithme.club.

## Установка

Нужна Switch с Atmosphere, её SD-карта и способ прочитать карту на компьютере
(или FTP), а также бесплатный аккаунт на [gowithme.club](https://gowithme.club).
Пошаговая инструкция с картинками — на gowithme.club в **Настройки → Nintendo
Switch (CFW)**, коротко:

1. На gowithme.club выпустите токен и скачайте архив. Токен в нём уже записан в
   `config/gowithme.club/config.json`.
2. Полностью выключите консоль (удерживать питание → Power Options → Turn Off;
   спящий режим не подходит) и вставьте SD-карту в компьютер.
3. Скопируйте папки `atmosphere` и `config` из архива в корень карты. На вопрос
   про совпадающие папки отвечайте **«Объединить»**, а не «Заменить».
4. Верните карту и загрузитесь в Atmosphere. Модуль запустится сам.
5. Запустите любую игру и оставьте её примерно на минуту. На gowithme.club она
   появится как «сейчас играет».

Архив лежит и на [странице релизов](https://github.com/gowithme-club/switch-playtime-tracker/releases/latest);
без токена модуль ничего не делает. Проверить или выключить его можно в оверлее
**Sysmodules** (Ultrahand / Tesla). Всё, что делает модуль, пишется в
`config/gowithme.club/log.txt` на карте.

Чтобы удалить модуль, сотрите с карты папку `atmosphere/contents/4200000000505341`.

## Что отправляется

Title ID и название игры, время начала и конца сессии, игровое время и ник
профиля на консоли, по которому сессии привязываются к вам.

## Где проверено

Живая Switch с Atmosphere. Проверенные версии — на
[странице релизов](https://github.com/gowithme-club/switch-playtime-tracker/releases).
Отчёты с других сборок очень пригодятся.

## Сборка из исходников

Нужен [devkitPro](https://devkitpro.org) с `switch-libjson-c` и `switch-curl`:

```bash
dkp-pacman -S switch-libjson-c switch-curl
make            # dist/atmosphere/contents/4200000000505341/
```

Ключи конфига, почему бюджет памяти определяет устройство кода и что измерено
на консоли — [docs/DEVELOPMENT.ru.md](docs/DEVELOPMENT.ru.md).

## Другие консоли

| Консоль | Репозиторий | Что ставится |
| --- | --- | --- |
| PS5 с джейлбрейком | [ps5-playtime-tracker](https://github.com/gowithme-club/ps5-playtime-tracker) | пейлоад |
| PS3 с HEN или CFW | [ps3-playtime-tracker](https://github.com/gowithme-club/ps3-playtime-tracker) | VSH-плагин |
| Switch с Atmosphere | **switch-playtime-tracker** (этот) | sysmodule |

Обычные консоли, Steam, Xbox и Discord подключаются на
[gowithme.club](https://gowithme.club) без установки чего-либо. Обычная Switch —
по Friend Code.

## Авторы кода и лицензия

[MIT](LICENSE). Каркас sysmodule взят из
[llsc12/SwitchRPC](https://github.com/llsc12/SwitchRPC) (MIT); чтение игровых
событий следует формату и алгоритму
[NX Activity Log](https://github.com/tallbl0nde/NX-Activity-Log).

Это неофициальный homebrew, не связанный с Nintendo и не одобренный ею. Nintendo
Switch — товарный знак Nintendo. Запуск homebrew на консоли — на ваш риск.
