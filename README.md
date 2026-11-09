<p align="center">
  <a href="https://gowithme.club"><img src=".github/assets/banner.png" alt="Switch playtime tracker by gowithme.club: sessions and hours from a Switch with Atmosphere" width="100%"></a>
</p>

<p align="center">
  <b>English</b> · <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <a href="https://github.com/gowithme-club/switch-playtime-tracker/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/gowithme-club/switch-playtime-tracker?style=flat-square&labelColor=131C16&color=C7F547&label=release"></a>
  <img alt="Switch with Atmosphere" src="https://img.shields.io/badge/Switch-Atmosphere-C7F547?style=flat-square&labelColor=131C16">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/badge/license-MIT-C7F547?style=flat-square&labelColor=131C16"></a>
  <a href="https://gowithme.club"><img alt="gowithme.club" src="https://img.shields.io/badge/profile-gowithme.club-C7F547?style=flat-square&labelColor=131C16"></a>
</p>

# Switch playtime tracker

An Atmosphere sysmodule for a modded Nintendo Switch that records what you play
and for how long and sends it to your profile on
[gowithme.club](https://gowithme.club), the cross-platform gaming stats tracker.
It starts with the system and runs in the background: no Nintendo Switch Online,
no traffic to Nintendo, no PC needed while you play.

The source is open so you can see exactly what runs on your console and what
leaves it.

## What you get

- **Playtime per game**: every session with its start, end and length, read from
  the console's own play journal.
- **Every profile on the console**: each user profile connects separately, so a
  shared Switch keeps everyone's stats apart.
- **Playing now**: friends see what you are playing, and a toast shows how long
  you played when you close a game.
- **Offline-safe**: days without internet are backfilled from the journal once
  the console is online again.
- **One profile for every console**: Switch, PS5, PS3, Steam, Xbox and PSN in one
  place on gowithme.club.

## How it works

```text
Switch ── sysmodule reads the system play journal (pdm)
            │
            └── HTTP ──► gowithme.club ──► your profile, friends, Telegram reports
```

The sysmodule only reports raw events. Game names, artwork, totals and
deduplication are handled by the gowithme.club server.

## Install

You need a Switch running Atmosphere, its SD card with a way to read it on a
computer (or FTP), and a free [gowithme.club](https://gowithme.club) account. The
step-by-step guide with pictures lives in **Settings → Nintendo Switch (CFW)** on
gowithme.club; in short:

1. On gowithme.club issue a token and download the archive. It already contains
   your token in `config/gowithme.club/config.json`.
2. Power the console off completely (hold power → Power Options → Turn Off;
   sleep will not do) and put the SD card into your computer.
3. Copy the `atmosphere` and `config` folders from the archive to the root of the
   card. When asked about existing folders, choose **Merge**, not Replace.
4. Put the card back and boot into Atmosphere. The module starts by itself.
5. Start any game and leave it running for about a minute. It shows up on
   gowithme.club as playing now.

The archive is also on the [releases page](https://github.com/gowithme-club/switch-playtime-tracker/releases/latest);
without a token the module stays idle. To check or toggle it, use the
**Sysmodules** overlay (Ultrahand / Tesla). Everything the module does is logged
to `config/gowithme.club/log.txt` on the card.

To remove it, delete `atmosphere/contents/4200000000505341` from the card.

## What is sent

The game's title ID and name, session start and end times, playtime, and the
console profile nickname used to match the sessions to you.

## Tested on

A live Switch with Atmosphere. Verified versions are listed in the [releases](https://github.com/gowithme-club/switch-playtime-tracker/releases).
Reports from other setups are welcome.

## Build from source

You need [devkitPro](https://devkitpro.org) with `switch-libjson-c` and
`switch-curl`:

```bash
dkp-pacman -S switch-libjson-c switch-curl
make            # dist/atmosphere/contents/4200000000505341/
```

The config keys, why the memory budget shapes the code, and what was measured on
the console: [docs/DEVELOPMENT.ru.md](docs/DEVELOPMENT.ru.md) (in Russian).

## Other consoles

| Console | Repository | Runs as |
| --- | --- | --- |
| PS5, jailbroken | [ps5-playtime-tracker](https://github.com/gowithme-club/ps5-playtime-tracker) | payload |
| PS3 on HEN or CFW | [ps3-playtime-tracker](https://github.com/gowithme-club/ps3-playtime-tracker) | VSH plugin |
| Switch with Atmosphere | **switch-playtime-tracker** (this one) | sysmodule |

Stock consoles, Steam, Xbox and Discord connect on
[gowithme.club](https://gowithme.club) without installing anything. A stock
Switch connects by Friend Code.

## Help and contributing

Questions and bug reports: [Telegram support](https://t.me/gowithme_club_support)
or [GitHub issues](https://github.com/gowithme-club/switch-playtime-tracker/issues).
Pull requests are welcome.

## Credits and license

[MIT](LICENSE). The sysmodule skeleton comes from
[llsc12/SwitchRPC](https://github.com/llsc12/SwitchRPC) (MIT); reading play events
follows the format and algorithm of
[NX Activity Log](https://github.com/tallbl0nde/NX-Activity-Log).

This is unofficial homebrew, not affiliated with or endorsed by Nintendo.
Nintendo Switch is a trademark of Nintendo. Running homebrew on your console is
at your own risk.
