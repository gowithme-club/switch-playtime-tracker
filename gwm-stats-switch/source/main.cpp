// gwm-stats-switch — Nintendo Switch sysmodule that reports play activity to a
// gwm-stats server over the ingest API (docs/ingest-api-v1.md).
//
// Derived from llsc12/SwitchRPC (MIT), which supplied the sysmodule skeleton:
// the npdm permissions, heap sizing, socket configuration, psc sleep/wake
// protocol and the HTTP `Date` clock correction. The Discord client it was
// built around is gone; what replaces it talks to our own ingest endpoints.
//
// This build implements the heartbeat stream (live "who is playing now").
// Closed sessions come from the console's own play records (pdm) and are added
// on top of the watermark this loop already reads back from the server.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>
#include <curl/curl.h>

#include <string>
#include <vector>

#include "config.hpp"
#include "ingest.hpp"
#include "logging.hpp"
#include "notify.hpp"
#include "play_journal.hpp"
#include "probe.hpp"
#include "utilities.hpp"
#include "version.hpp"

// Every byte here is reserved for the life of the console — it is a static
// array, not a high-water mark — and it comes out of the pool the system also
// draws on when a game opens an applet. On this console 0x80000 (inherited from
// SwitchRPC, which had no reason to be frugal) made the album and Ni No Kuni's
// privacy-policy browser fail to open: hid died with kernel LimitReached and
// took the game with it, at the same instruction every time, and the game was
// fine the moment the sysmodule was switched off. Halving it was not enough —
// the browser appeared and died a second later — which is what sent libcurl and
// its TLS stack out of this module altogether.
//
// What has to fit: the socket driver's transfer memory (see the formula at
// socketInitialize — 128 KB), curl, json-c, the psc thread's stack, a 0x4000
// control-data read, and a response that never reaches a kilobyte. 0x20000 was
// one step too far — the psc thread could not get its stack, and the module
// died quietly at startup. Measured here: 201 KB in use at boot, peaking at
// 223 KB during a journal backfill.
//
// 0x80000 was tried again, deliberately, because it is the only size that can
// hold the 144 KB control-data read that yields a game's name, and because the
// evidence against it had been gathered while this module was also wrong about
// several other things. The album opened, which the old evidence said it would
// not. Ni No Kuni's browser did not: the console froze on the applet opening,
// with the game's music still playing. Not the old crash — hid dying at
// 2001-0132 and taking the game with it — but a hang, which is what an applet
// refused its memory does instead.
//
// So the ceiling is real, it is somewhere below 0x80000, and it is worth more
// than a game's name. Names are the server's job now; see getTitleName.
#define INNER_HEAP_SIZE 0x50000

#ifdef __cplusplus
extern "C" {
#endif

// Sysmodules must not use applet*.
u32 __nx_applet_type = AppletType_None;

// Sysmodules normally only want one FS session.
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    timeInitialize();
    nifmInitialize(NifmServiceType_System);
    fsdevMountSdmc();
    initLog();

    // These numbers are not "how much memory the sockets may use". They are the
    // socket driver's own working memory, and libnx sizes a transfer memory
    // block from them, exactly:
    //
    //   tmem = page_up(tcp_tx_max + tcp_rx_max + udp_tx + udp_rx) × sb_efficiency
    //
    // libnx's own comment on that formula is the whole story of this bug: below
    // the size the config implies, "the BSD sockets service would only send
    // ZeroWindow packets (for TCP), resulting in a transfer rate not exceeding
    // 1 byte/s". Shrinking these values to save memory did not make the module
    // send slowly — it made send() never return at all. A 300-byte POST into an
    // established connection sat in one send() call for 63 seconds and counting
    // (0.3.9's probe), which is also precisely how curl hangs here: curl's
    // CURLOPT_TIMEOUT is checked between waits, and a send that never comes back
    // never lets it be checked.
    //
    // So these are SwitchRPC's values, which are known to carry heavier traffic
    // than this module will ever produce, on this same hardware: 0x8000 × 4 =
    // 128 KB of transfer memory. It is a lot, and it is the thing being tested.
    // Trim it later by measurement, not by eye — the previous trim cost weeks.
    SocketInitConfig sockConf = {
        .tcp_tx_buf_size = 0x800,
        .tcp_rx_buf_size = 0x1000,
        .tcp_tx_buf_max_size = 0x2EE0,
        .tcp_rx_buf_max_size = 0x2EE0,

        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x1000,

        .sb_efficiency = 4,

        .num_bsd_sessions = 2,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&sockConf);
    if (R_FAILED(rc)) {
        writeToLog(
            "[Init] socketInitialize failed (0x%08X) — probably memory pressure. "
            "Nothing will be reported. Try disabling other sysmodules.",
            rc);
    } else {
        writeToLog("[Init] Socket driver up on bsd:s, %u session, sb_efficiency %u.",
                   (unsigned)sockConf.num_bsd_sessions, (unsigned)sockConf.sb_efficiency);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    pmdmntInitialize();
    nsInitialize();
    setInitialize();  // nacpGetLanguageEntry needs the system language
    // acc and pdm:qry are deliberately *not* opened here — see openUserServices.
    pscmInitialize();

    smExit();
}

void __appExit(void) {
    g_pscThreadRun = false;
    if (g_pscReady) {
        threadWaitForExit(&g_pscThread);
        threadClose(&g_pscThread);
        pscPmModuleFinalize(&g_pscModule);
        pscPmModuleClose(&g_pscModule);
    }
    pscmExit();

    writeToLog("[Exit] Sysmodule stopping.");

    curl_global_cleanup();
    socketExit();
    closeUserServices();
    setExit();
    nsExit();
    pmdmntExit();
    nifmExit();
    timeExit();

    fsdevUnmountAll();
    fsExit();
}

#ifdef __cplusplus
}
#endif

// How long to wait for the launching profile to show itself before settling for
// whoever the console already had open. A game opens its user during its own
// startup — splash screens first — so the answer is not there the moment the
// title appears; it usually lands within a few seconds and always well inside a
// minute.
#define USER_GRACE_SEC 60

// How far before the launch a play-journal entry may be stamped and still count
// as this session's. The profile is picked before the game starts, the module
// notices the title up to a poll late, and the console clock is its own idea of
// time — but a previous launch of the same game is minutes or days away, not
// two, so this separates them without being tight.
#define PDM_LAUNCH_SLACK_SEC 120

// How long to leave the journal backfill alone after a round that changed
// nothing. Long enough that a batch the server refuses stops costing a full
// journal scan every cycle, short enough that a server-side fix takes effect
// the same evening without touching the console.
#define BACKFILL_PAUSE_SEC 600

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // The heap size identifies the build in the log: this module is tuned
    // against a limit that is only visible on the console, and a test run that
    // cannot name the build it ran proves nothing.
    writeToLog("[Init] %s started (heap %u KB).", GWM_STATS_SWITCH_CLIENT,
               (unsigned)(INNER_HEAP_SIZE / 1024));
    // What the socket driver's transfer memory left behind. This is the budget
    // everything else in the module has to fit in.
    logHeapUsage("after socket init");

    IngestConfig cfg;
    loadConfigIfChanged(&cfg);

    AppInfo lastInfo = {};
    u64 accumulatedAwake = 0;  // awake seconds already banked for the current game
    u64 chunkStartRaw = 0;     // raw clock when the current awake chunk started
    u64 lastHeartbeatRaw = 0;
    u64 backfillPausedUntilRaw = 0;  // set when a backfill round changes nothing
    std::string watermark;
    std::string sessionUser;      // profile of the running session, empty = use the config's account
    bool sessionUserSettled = true;  // false while we are still waiting for the launching profile
    AccountUid userBeforeLaunch = {};
    u64 userGraceUntilRaw = 0;
    u64 launchRawSec = 0;  // earliest play-journal stamp that can belong to this session

    waitForNetworkReady();

    // Register with psc and start the thread that watches for sleep/wake.
    {
        ueventCreate(&g_pscWakeMain, true);
        ueventCreate(&g_sleepPrepDone, true);
        static const u32 pscDeps[] = {PscPmModuleId_Fs, PscPmModuleId_Nifm};
        Result prc = pscmGetPmModule(&g_pscModule, (PscPmModuleId)0x5250, pscDeps,
                                     sizeof(pscDeps) / sizeof(pscDeps[0]), true);
        if (R_SUCCEEDED(prc)) {
            if (R_SUCCEEDED(threadCreate(&g_pscThread, pscThreadFunc, NULL, NULL, 0x4000, 0x2C, -2))) {
                threadStart(&g_pscThread);
                g_pscReady = true;
                writeToLog("[Init] PSC power-state monitoring active.");
            } else {
                // Without the ack thread we would stall power transitions.
                pscPmModuleFinalize(&g_pscModule);
                pscPmModuleClose(&g_pscModule);
                writeToLog("[Init] Warning: could not start the PSC thread; sleep detection is off.");
            }
        } else {
            writeToLog("[Init] Warning: pscmGetPmModule failed (0x%08X); sleep detection is off.", prc);
        }
    }

    // Only after psc is up and the loop is about to start: in 0.3.9 the probe
    // ran on this thread, before all of that, and when it wedged the module
    // could not be switched off from Sysmodules at all — the console had to be
    // rebooted. It now runs on its own thread and only when armed by a marker
    // file, so at worst it loses itself, not the module.
    if (cfg.valid) startTransportProbe(cfg);

    while (true) {
        bool forceHeartbeat = false;
        bool sessionJustNamed = false;

        // Just woke up: the network stack needs a moment, and the game that was
        // running is still running.
        if (g_wokeFromSleep) {
            g_wokeFromSleep = false;
            g_asleep = false;
            waitForNetworkReady();

            if (lastInfo.tid != 0) {
                writeToLog("[Loop] Woke up, resuming %016llX", (unsigned long long)lastInfo.tid);
                chunkStartRaw = getRawNowSec();
            }
            forceHeartbeat = true;
        }

        // Config is re-read whenever the file changes, so the URL or token can
        // be fixed without rebooting the console.
        writeTrace("loop: loadConfigIfChanged");
        if (loadConfigIfChanged(&cfg)) {
            forceHeartbeat = cfg.valid;
        }

        if (!g_asleep && cfg.valid) {
            AppInfo info = {};
            writeTrace("loop: get_app_info");
            get_app_info(&info);
            writeTrace("loop: app info read");

            // tid is filled before the name lookup, so a title with no control
            // data still counts as playing — the name is what goes missing.
            const bool playing = info.tid != 0;

            if (playing && info.tid != lastInfo.tid) {
                lastInfo = info;
                accumulatedAwake = 0;
                chunkStartRaw = getRawNowSec();

                // Whoever the console has open right now is the *previous*
                // player: the new game has not opened its user yet. Keep it as
                // the baseline — a change away from it is the launch we want,
                // and no change means the same person started another game.
                sessionUser.clear();
                sessionUserSettled = false;
                userBeforeLaunch = AccountUid{};

                writeTrace("launch: openUserServices for the baseline profile");
                if (openUserServices()) {
                    writeTrace("launch: accountGetLastOpenedUser");
                    getCurrentUserUid(&userBeforeLaunch);
                    writeTrace("launch: closeUserServices");
                    closeUserServices();
                }
                writeTrace("launch: baseline profile done");

                userGraceUntilRaw = chunkStartRaw + USER_GRACE_SEC;
                launchRawSec = chunkStartRaw > PDM_LAUNCH_SLACK_SEC ? chunkStartRaw - PDM_LAUNCH_SLACK_SEC : 0;

                writeToLog("[Loop] Now playing %016llX (%s), waiting for the profile", (unsigned long long)info.tid,
                           info.title_name);
            } else if (!playing && lastInfo.tid != 0) {
                writeToLog("[Loop] Stopped playing %016llX", (unsigned long long)lastInfo.tid);
                lastInfo = AppInfo{};
                sessionUser.clear();
                sessionUserSettled = true;
                accumulatedAwake = 0;
                chunkStartRaw = 0;
                forceHeartbeat = true;
            }

            if (lastInfo.tid != 0 && !sessionUserSettled) writeTrace("profile: openUserServices");

            if (lastInfo.tid != 0 && !sessionUserSettled && openUserServices()) {
                writeTrace("profile: services open");
                const bool graceOver = getRawNowSec() >= userGraceUntilRaw;
                AccountUid uid = {};
                const char* how = NULL;
                writeTrace("profile: getUserForTitle (pdm scan)");

                // In order of authority. pdm ties the account to the very
                // application, so it is the only source that cannot be confused
                // by whatever happened just before the launch.
                if (getUserForTitle(lastInfo.tid, launchRawSec, &uid)) {
                    how = "play journal";
                } else if ((writeTrace("profile: getOnlyRegisteredUser (acc ListAllUsers)"),
                            getOnlyRegisteredUser(&uid))) {
                    how = "the console's only profile";
                } else if ((writeTrace("profile: getSingleOpenUser (acc ListOpenUsers)"), getSingleOpenUser(&uid))) {
                    how = "the only open profile";
                } else if ((writeTrace("profile: accountGetLastOpenedUser"), getCurrentUserUid(&uid)) &&
                           memcmp(&uid, &userBeforeLaunch, sizeof(uid)) != 0) {
                    how = "a profile opened after the launch";
                }
                writeTrace("profile: sources consulted");

                if (how != NULL || graceOver) {
                    char selectedUid[33] = {};

                    if (how == NULL) {
                        // Out of grace with nothing better: whoever was open
                        // when the game started, which is right whenever the
                        // same person simply started another game.
                        how = "nothing better than the profile from before";
                        uid = userBeforeLaunch;
                    }

                    if (!accountUidIsValid(&uid) &&
                        (writeTrace("profile: getConfiguredDefaultUser (config defaultUid)"),
                         getConfiguredDefaultUser(cfg.defaultUid.c_str(), &uid))) {
                        // Nobody was open before the launch either, so there is
                        // no evidence left to weigh — the console has simply
                        // never been told who is at it. defaultUid is the owner
                        // answering that question in advance, which beats
                        // reporting the session under the config account as if
                        // no profile existed.
                        how = "the configured default profile";
                    }

                    UserInfo user = {};
                    writeTrace("profile: getUserInfo (nickname)");
                    if (getUserInfo(&uid, &user)) {
                        sessionUser = resolveServerAccount(cfg, &user);
                        memcpy(selectedUid, user.uid_string, sizeof(selectedUid));
                    }

                    sessionUserSettled = true;
                    // The session is only reported once it has an owner, so the
                    // wait costs a little live-status latency and nothing else:
                    // startedAt is measured from the launch either way.
                    forceHeartbeat = true;
                    sessionJustNamed = true;

                    char sources[160];
                    writeTrace("profile: describeUserSources (re-scans pdm and acc)");
                    describeUserSources(lastInfo.tid, launchRawSec, sources, sizeof(sources));
                    writeToLog("[Loop] Playing as %s uid=%s, by %s (%s)",
                               sessionUser.empty() ? "<unknown, using the config account>" : sessionUser.c_str(),
                               selectedUid[0] != '\0' ? selectedUid : "-", how, sources);
                }

                writeTrace("profile: closeUserServices");
                closeUserServices();
                writeTrace("profile: done");
            }

            const u64 nowRaw = getRawNowSec();
            const bool due = nowRaw - lastHeartbeatRaw >= (u64)GWM_HEARTBEAT_SECONDS;

            if (forceHeartbeat || due) {
                u64 startedAt = 0;
                // A session with no owner yet is not reported at all. Naming the
                // wrong profile and correcting it a minute later would leave the
                // server holding a real, if short, session for somebody who was
                // not playing — worse than a late badge.
                const bool report = lastInfo.tid != 0 && sessionUserSettled;

                if (report) {
                    // Sleep is excluded: only awake seconds count towards the
                    // "playing for N minutes" the dashboard shows.
                    const u64 awake = accumulatedAwake + (nowRaw - chunkStartRaw);
                    startedAt = getCorrectedNowSec() - awake;
                }

                // "switch" is the fallback, not the answer: it names the
                // console when the profile could not be read.
                const std::string account = sessionUser.empty() ? std::string("switch") : sessionUser;

                writeTrace("heartbeat: POST /ingest/v1/heartbeat");
                std::string serverTitleName;
                if (ingestHeartbeat(cfg, report ? &lastInfo : NULL, startedAt, account, &watermark,
                                    &serverTitleName)) {
                    writeTrace("heartbeat: accepted");
                    lastHeartbeatRaw = nowRaw;

                    // Once per launch, after the owner is known and the first
                    // heartbeat landed. Not on wake or config reload — those
                    // also force a heartbeat, and a toast every sleep would
                    // drown the ones that mean something.
                    // The name is the server's: this console cannot read its
                    // own NACP, and the server resolves the id through titledb
                    // on every heartbeat. Our own NACP read is second, the bare
                    // id last.
                    if (sessionJustNamed && report) {
                        char text[128];
                        if (!serverTitleName.empty()) {
                            snprintf(text, sizeof(text), "Играет: %.60s", serverTitleName.c_str());
                        } else if (lastInfo.title_name[0] != '\0') {
                            snprintf(text, sizeof(text), "Играет: %.60s", lastInfo.title_name);
                        } else {
                            snprintf(text, sizeof(text), "Играет: %016llX", (unsigned long long)lastInfo.tid);
                        }
                        notifyUltrahand(cfg, text);
                    }

                    // PDM is the durable offline buffer. Upload a few oldest
                    // closed slices after each successful heartbeat; every
                    // response advances the server-owned watermark, so a long
                    // offline backlog drains incrementally and retries are
                    // naturally idempotent.
                    // A backfill round that moves nothing is not free: each one
                    // rescans the whole pdm journal, posts the same sessions
                    // again and fragments the heap that the next request has to
                    // allocate out of. When the server rejects a batch the
                    // watermark stays put, and without this the module resent
                    // the same two sessions every thirteen seconds forever.
                    if (backfillPausedUntilRaw != 0 && nowRaw < backfillPausedUntilRaw) {
                        writeTrace("journal: backfill paused");
                    } else {
                        bool madeProgress = false;
                        IngestBatchResult drained;  // summed over the batches below
                        // Two sessions per POST, one when two did not go: a
                        // long session is up to 128 events, and a pair of them
                        // can be more than the heap has left for curl.
                        size_t batchSize = 2;
                        for (int batch = 0; batch < 3; batch++) {
                            std::vector<PlayJournalSession> pending;
                            writeTrace("journal: collectPendingPlaySessions (full pdm scan)");
                            if (!collectPendingPlaySessions(cfg, watermark, batchSize, &pending) || pending.empty()) {
                                madeProgress = true;  // nothing owed, not stuck
                                break;
                            }

                            const std::string previousWatermark = watermark;
                            // The scan that just ran is the heaviest thing this
                            // module does to its heap, and the POST after it is
                            // where curl first failed to allocate. Measure here.
                            logHeapUsage("after the journal scan");
                            writeTrace("journal: POST /ingest/v1/switch/play-sessions");
                            IngestBatchResult batchResult;
                            const size_t sent = pending.size();
                            if (!ingestPlaySessions(cfg, &pending, &watermark, &batchResult)) {
                                if (sent <= 1) break;
                                writeToLog("[Journal] A batch of %u session(s) did not go; retrying one at a time.",
                                           (unsigned)sent);
                                batchSize = 1;
                                continue;
                            }
                            drained.accepted += batchResult.accepted;
                            drained.recent += batchResult.recent;
                            drained.totalSeconds += batchResult.totalSeconds;
                            for (const IngestStoredSession& row : batchResult.kept) {
                                if (drained.kept.size() >= IngestBatchResult::MAX_KEPT) break;
                                drained.kept.push_back(row);
                            }
                            if (watermark == previousWatermark) break;
                            madeProgress = true;
                        }

                        // A few sessions get a toast each, named and timed by
                        // the server. A long backfill gets one line with the
                        // total: Ultrahand shows three toasts at once and
                        // queues the rest, and a week offline is not worth a
                        // queue of forty. History older than RECENT_SEC gets
                        // nothing at all — that is the journal draining, which
                        // the person did not just do.
                        if (drained.recent > 0) {
                            char text[128];
                            char span[20];
                            if (drained.accepted <= (int)IngestBatchResult::MAX_KEPT &&
                                drained.kept.size() == (size_t)drained.accepted) {
                                // Same shape as the Telegram session card: the
                                // game on its own line, «7 мин · всего 41 мин»
                                // under it — Ultrahand breaks the text at '\n'.
                                // The total is the server's, across every
                                // platform the person plays on, and is left out
                                // when the server could not count it.
                                for (const IngestStoredSession& row : drained.kept) {
                                    if (!row.recent) continue;
                                    formatPlaySpan(row.seconds, span, sizeof(span));
                                    if (row.totalSeconds > 0) {
                                        char total[20];
                                        formatPlaySpan(row.totalSeconds, total, sizeof(total));
                                        snprintf(text, sizeof(text), "%.60s\n%s · всего %s", row.titleName.c_str(), span,
                                                 total);
                                    } else {
                                        snprintf(text, sizeof(text), "%.60s\n%s", row.titleName.c_str(), span);
                                    }
                                    notifyUltrahand(cfg, text);
                                }
                            } else {
                                formatPlaySpan(drained.totalSeconds, span, sizeof(span));
                                snprintf(text, sizeof(text), "Сессий ушло: %d, всего %s", drained.accepted, span);
                                notifyUltrahand(cfg, text);
                            }
                        }

                        if (madeProgress) {
                            backfillPausedUntilRaw = 0;
                        } else {
                            backfillPausedUntilRaw = nowRaw + BACKFILL_PAUSE_SEC;
                            writeToLog(
                                "[Journal] Backfill made no progress; pausing it for %d minutes. "
                                "Heartbeats continue.",
                                BACKFILL_PAUSE_SEC / 60);
                        }
                    }
                    writeTrace("journal: backfill done");
                } else {
                    writeTrace("heartbeat: failed");
                    // Retry sooner than the full interval — a failed heartbeat
                    // usually means the NAS or the wifi blinked.
                    const u64 retryDelay = 30;
                    lastHeartbeatRaw = (u64)GWM_HEARTBEAT_SECONDS > retryDelay
                                           ? nowRaw + retryDelay - (u64)GWM_HEARTBEAT_SECONDS
                                           : nowRaw;
                }
            }
        }

        // Sleep ~10s, or wake early when psc pokes us.
        writeTrace("loop: idle");
        waitSingle(waiterForUEvent(&g_pscWakeMain), 10ULL * 1000 * 1000 * 1000);

        // About to sleep with wifi still up: this is the only moment we can tell
        // the server the console is going away. Otherwise live status would sit
        // there until the server's 5 minute TTL expires it.
        if (g_sleepPending) {
            g_sleepPending = false;
            writeToLog("[Loop] Sleep imminent, flushing an idle heartbeat.");

            if (lastInfo.tid != 0) {
                accumulatedAwake += getRawNowSec() - chunkStartRaw;
            }

            if (cfg.valid) {
                ingestHeartbeat(cfg, NULL, 0, "switch", &watermark);
            }

            g_asleep = true;
            ueventSignal(&g_sleepPrepDone);
        }
    }

    return 0;
}
