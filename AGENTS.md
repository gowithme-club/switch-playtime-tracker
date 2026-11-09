# Repository Guidelines

Canonical instructions for any agent working in this repository. `CLAUDE.md` only points here.

## What this is

One Atmosphere sysmodule for a CFW Nintendo Switch — `gwm-stats-switch/`, Title ID `4200000000505341`, versioned by `VERSION` at the repo root — which reports play activity to a gwm-stats server. `make` lays it out as an SD-card image under `dist/atmosphere/`, dragged onto the card root as-is.

A second module, `gwm-reloader` (`4200000000505342`), used to ship alongside it to restart this one over LAN TCP. It was deleted because its listener wedged in a `poll` that never returned. It was also blamed for hanging this module's HTTP calls, and that was wrong — see the socket section below for what actually caused it. It is in git history.

The server contract is `docs/ingest-api-v1.md` in the gwm-stats repo, with a client-author version at `docs/ingest-api-v1-client.md`. `README.md` (English) and `README.ru.md` are the short public pages for people installing the module — keep them in step with each other and with the site's setup guide; `docs/DEVELOPMENT.ru.md` (Russian) holds the full install, config and reasoning; code comments and commit messages are English.

## Sibling repositories

This module is one client of a server that owns all the domain logic — dedup, sessions, trophies, live status, dashboard. A client only forwards events over the ingest API; anything that changes server behaviour or the contract itself belongs in gwm-stats, not here.

- gwm-stats — server, ingest endpoints, web dashboard, public read API.
- [ps3-playtime-tracker](https://github.com/gowithme-club/ps3-playtime-tracker) — the PS3 VSH plugin (CFW / PS3HEN), a second client of the same ingest API. Same three flows on a different console, so it is the place to compare how heartbeat, closed sessions and trophies are supposed to behave.
- gwm-stats-ios — SwiftUI app that reads the server's public API; it never writes to ingest.

## Build

devkitPro is installed locally on this machine (`/opt/devkitpro`, installer from the `devkitPro/pacman` releases — there is no Homebrew cask). It needs `switch-dev`, `switch-libjson-c` and `switch-curl`:

```bash
sudo dkp-pacman -Sy --noconfirm switch-dev switch-curl switch-libjson-c
```

The toolchain is not on `PATH`, so builds pass it in:

```bash
DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH="/opt/devkitpro/tools/bin:$PATH" make
```

Builds used to run in a `devkitpro/devkita64` container. If `build/` still holds dependency files from that era they name `/src/...` paths and make fails with "No rule to make target"; `rm -rf gwm-stats-switch/build dist` clears it.

There is no CI and no container recipe in the repo — that was deliberate, do not add one back without being asked. `make` from the repo root builds the module and lays out `dist/atmosphere/`, which is dragged onto the SD card root as-is. `make -C gwm-stats-switch` builds without the dist step.

`dist/` and `build/` are gitignored; the binaries in the working tree are build output, not sources.

**Version bumps touch two files.** `check-version` runs before anything else and fails the build unless `VERSION` matches `"version"` in `gwm-stats-switch/toolbox.json`. The Makefile generates `build/version.hpp` from `VERSION`; that string is the first line of the log and the `User-Agent` of every request as `switch-plugin/<version>`, the bare `X-Client-Version` (the server shows it next to the device on its status page, which already names the platform), and the label next to the module in the Sysmodules UI. Bump it whenever on-console behaviour changes — a test run that cannot name the build it ran proves nothing.

## Testing

There is no test suite and no host-runnable code — everything depends on Horizon services. Verification is a real console: install, then read `sdmc:/config/gowithme.club/log.txt`. Compiling in the container is the only check available from a workstation.

The console's SD card is reachable over FTP at `ftp://192.168.1.37:5000` — that is how new builds go over and how logs and configs are read back without touching the card:

```bash
curl -s ftp://192.168.1.37:5000/config/gowithme.club/log.txt | tail -40
```

```bash
curl -T gwm-stats-switch/gwm-stats-switch.nsp ftp://192.168.1.37:5000/atmosphere/contents/4200000000505341/exefs.nsp
```

Restarting the module afterwards is a manual step by the user in the Sysmodules UI — there is no remote restart. Ask for it and wait; do not assume a new build is running until its version appears in the log's first line.

Keep a copy of what is being replaced. The console holds `exefs.nsp.pre-<version>` backups made by renaming over FTP (`curl -Q "RNFR …" -Q "RNTO …"`), which is what makes a bad build one rename away from a rollback.

## The memory budget shapes most of this code

A sysmodule's heap is reserved for the life of the console and comes out of the same pool the system uses to give a game's **applet** memory. When it runs short, the failure lands on the game opening a browser or the album — not on this module. This was reproduced on a live console (Ni No Kuni's privacy-policy screen, `hid` dying with `2001-0132` LimitReached, fixed by switching the sysmodule off).

Consequences that will look like premature optimization if you do not know why:

- `INNER_HEAP_SIZE` is `0x50000`, and the working range is now bounded from both ends by measurement. `0x20000` was too small — the psc thread could not get its stack. `0x80000` is too large — the album opens, but Ni No Kuni's in-game browser freezes the console with the game's music still playing, which is what an applet does when refused its memory. At `0x50000` both open. It is a `static` array: every byte is reserved for the life of the console whether it is used or not, so there is no such thing as a "temporary" allocation here. The reservation is what costs, not the peak.
- No TLS. `http://` only; `caBundle` in old configs is ignored. Rejected in `config.cpp` at load time (not in the transport) so a bad URL fails once, loudly, instead of on every POST. `ingestUrl` itself is optional since 1.2.0: it defaults to `GWM_DEFAULT_INGEST_URL` (`http://gowithme.club`) and stays only as an override for a server run at home. `account` and `heartbeatSeconds` were removed the same way — the fallback profile name is always `switch`, the interval is `GWM_HEARTBEAT_SECONDS`; a person supplies a token and nothing else.
- `acc` and `pdm:qry` are opened and closed around each lookup (`openUserServices`/`closeUserServices`) rather than held from boot.

Treat any change that grows the heap or held service sessions as a change that can crash unrelated games, and say so in the commit message. But read the next section first: cutting memory here has already cost more than it saved, twice.

## The socket buffers are not a cache — they are the driver, and starving them is silent

For most of this project the module never sent a single byte to the server. Every transport connected in about six milliseconds and then stopped: the hand-written one, and curl after it. curl's `CURLOPT_TIMEOUT` of twelve seconds never fired, which is the tell — curl checks its timeouts *between* waits, so a timeout that never fires means a call that never returned. An on-console probe found it: one `send()` of 300 bytes into an established connection did not return in 63 seconds.

The cause was `SocketInitConfig`. libnx turns those numbers into a transfer memory block, exactly:

```
tmem = page_up(tcp_tx_max + tcp_rx_max + udp_tx + udp_rx) × sb_efficiency
```

and its own comment on that function is the whole story: below the size the config implies, *"the BSD sockets service would only send ZeroWindow packets (for TCP), resulting in a transfer rate not exceeding 1 byte/s"*. Starved, the driver does not go slow. It stops, inside `send`, with no error and no timeout.

The configurations that hung were 24 KB and 28 KB of transfer memory. The current values are SwitchRPC's — `0x8000 × 4` = 128 KB — and they work. **Do not trim them to save heap.** If they must be trimmed, move `sb_efficiency` one step at a time and verify a POST on the console after each step.

Three things that were written here as fact, and are not:

- `num_bsd_sessions` does **not** enter the formula. Sessions are nearly free; `sb_efficiency` and the buffer sizes are what cost.
- `poll` on this console honours its timeout. Measured: 1011 ms for a 1000 ms wait.
- `fcntl(F_SETFL, O_NONBLOCK)` takes effect. `F_GETFL` reads back `0x4000` afterwards.

`logHeapUsage` prints bytes in use, bytes free and the block count at three points — after socket init, after a journal scan, and when curl cannot allocate. Use it. Note that `mallinfo` only sees the arena newlib has taken so far, not all of `INNER_HEAP_SIZE`. Measured with 128 KB of transfer memory: 201 KB in use at boot, peaking at 223 KB during a backfill.

curl's own buffers are set explicitly (`CURLOPT_UPLOAD_BUFFERSIZE` 16 KB, its floor, and `CURLOPT_BUFFERSIZE` 4 KB). Left alone it wants 64 KB and 16 KB in single contiguous pieces, and after a journal scan the heap is too fragmented to give it that — `curl_easy_perform` returned `CURLE_OUT_OF_MEMORY` with 93 KB free in eight pieces.

The play-sessions body is written straight into one pre-sized string, and the sessions are freed before curl runs (`buildPlaySessionsBody` in `ingest.cpp`). A json-c tree costs a hash table per object; for two long sessions (up to 128 events each) the tree plus its printbuf left 26 KB for curl, and SMG2 sessions failed with `CURLE_OUT_OF_MEMORY` on every attempt, reboot included (2026-09-23 → 1.2.1). If a pair still fails, `main.cpp` retries one session at a time.

`sdmc:/config/gowithme.club/probe.txt` arms a one-shot transport probe (`probe.cpp`) that measures connect, send, recv and poll separately, on its own thread. It deletes the marker before running, so a wedged probe cannot arm itself again. Never run it on the main thread: an earlier version did, wedged before psc registration, and the module could not be switched off from Sysmodules at all — the console had to be rebooted.

## Games are reported without names

`nsGetApplicationControlData` is asked for `sizeof(NacpStruct)` (0x4000) — the nacp without the 0x20000 icon that follows it — and **this console refuses that size**, answering `0x5010` for installed titles and the running one alike. Titles therefore reach the server as bare IDs.

This has been settled experimentally, so do not re-open it without new information. The full 144 KB `NsApplicationControlData` read that SwitchRPC uses works: it named twenty titles on the first journal scan, including the running one. It is not used, because it only fits in a `0x80000` heap and that heap hangs the browser applet (see `INNER_HEAP_SIZE`). No intermediate exists — `INNER_HEAP_SIZE` is reserved permanently, so a 144 KB allocation "only when needed" still means reserving it always.

Names therefore belong to the server. [blawar/titledb](https://github.com/blawar/titledb) maps ids to names and box art — `010055D009F78000` is "Fire Emblem™: Three Houses" — in per-region files (`US.en.json` is 87 MB), so it is imported once rather than queried per request. Keys are base title ids, which is what `pmdmnt` gives us.

What remains here is defensive and costs nothing. `getTitleName` gives up after three refusals rather than repeating the discovery for every session in a journal scan, which was 793 service calls and 793 identical log lines in a single pass, and it caches per title id — including titles that cannot be named. A returned name is validated before use: it must end inside its own 0x200 field, contain no control bytes and be valid UTF-8. That last check is not paranoia — `01007EF00011E000` returns *success* with 309 bytes of binary where its name should be, because a `NacpLanguageEntry` name is a fixed field rather than a string. Real names include ™, ® and Cyrillic, so ASCII is not the test.

## Session attribution is the other hard-won invariant

The console is shared, so each session belongs to a profile, not to the console. `accountGetLastOpenedUser` names the **previous** player: a game opens its user partway through its own startup, later than the module notices the title. So sources are tried in order of authority in `main.cpp` — the `pdm` journal (which ties account to application id), then the console's only registered profile, then the single open profile, then a profile opened after the launch — inside a 60 s grace window, and the journal search is always bounded by launch time, because the journal remembers every launch the console ever saw and would otherwise answer with whoever played that game last week.

Until a session has an owner it is not reported at all. A late badge is better than a real session filed under someone who was not playing.

Two of those sources are the console's *standing* answer rather than evidence about this launch, and they exist because for some titles no evidence is ever produced. A homebrew port or a forwarder never asks which profile to use, so it never opens one, so `pdm` records no account event and no per-user statistics for it — the hunt cannot succeed, not this time and not on the next launch either. `getOnlyRegisteredUser` answers when the console has exactly one profile, where there is nobody else the session could belong to, and `getConfiguredDefaultUser` answers from `defaultUid` in the config, which is the owner deciding in advance. Neither is a guess at whoever is at hand, which is the thing the invariant above forbids. `defaultUid` is matched against the console's own profile list rather than parsed and trusted, so a typo disables the fallback instead of inventing a person. The same pair is consulted in `play_journal.cpp` when a closed session carries no account, and without them such a session is still dropped.

## Data flow

`main.cpp` runs one loop, waking every ~10 s or on a psc power event:

1. `pmdmnt` says which application is running; a title change resets the session and starts the profile hunt.
2. `POST /ingest/v1/heartbeat` — live "who is playing now". The response carries `sessionsWatermark`, the server-owned cursor.
3. After each successful heartbeat, up to 3 batches of closed sessions are read out of the console's PDM journal (`play_journal.cpp`) and sent to `POST /ingest/v1/switch/play-sessions`. This is the offline buffer: days without network are not lost, the journal stays on the console and drains later.

Play sessions are sent as **raw launch/focus/exit/account events**. Duration, owner selection, filtering and deduplication are the server's job — keep it that way rather than computing them here.

Sleep and wake come from `psc`, not from clocks (the module is frozen while the console sleeps). `PscPmState_ReadySleep` fires while wifi is still up and is the one chance to flush an empty heartbeat so live status goes out immediately instead of expiring on the server's 5-minute TTL. Sleep time is excluded from session duration.

The console RTC drifts on CFW and the server rejects future timestamps, so every timestamp sent is corrected by `g_timeOffset`, learned from the `Date` header of each response. `getRawNowSec()` measures intervals; `getCorrectedNowSec()` is what goes on the wire.

## Releases

A release is what a person starting from nothing actually installs, so its notes carry the whole installation — hardware required, which folder goes where, what the folder-merge prompt means, the config file, how to check it worked — not a changelog fragment. The «Установка с нуля» section of `docs/DEVELOPMENT.ru.md` and the release notes say the same thing in the same order; when one changes, so does the other.

Build clean from the tag, zip `dist/` as `gowithme-club-switch-<version>.zip` so the archive opens onto `atmosphere/` and `README.txt` (the site's `/download/switch-cfw` serves whichever `.zip` the latest release carries, and the site adds `config/gowithme.club/config.json` with the person's token when the archive is downloaded together with issuing one). `README.txt` is rendered from `README.dist.txt` at the repo root — the short Russian install-and-support note a person finds inside the archive; keep it in step with the README's install steps and the site's guide. Publish the SHA-256 of `exefs.nsp` next to it, and name in the notes which build was actually run on a console.

## Origin

Derived from [llsc12/SwitchRPC](https://github.com/llsc12/SwitchRPC) (MIT): npdm permissions, heap sizing, socket config, the psc protocol and `Date` clock correction. The PDM reading follows [NX Activity Log](https://github.com/tallbl0nde/NX-Activity-Log)'s session model. Keep the MIT LICENSE intact.

## Workflow

Commit straight to `main` — no feature branches in this repo.
