#include "play_journal.hpp"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "logging.hpp"
#include "utilities.hpp"

#define PDM_READ_WINDOW 32
#define MAX_EVENTS_PER_SESSION 128

// How far before the server's watermark the scan starts, in seconds and then in
// entries. Both are slack, for the same reason: a session is opened by a launch
// entry that comes before the exit the watermark is compared against, so
// starting exactly at the watermark would meet closing entries with no session
// open and drop them. Fourteen days covers a game left running — or asleep,
// which logs no exit at all — across a fortnight; the extra entries cover the
// user clock being moved backwards, which is the one thing that can put the
// journal briefly out of order.
#define SCAN_START_LOOKBACK_SEC (14 * 24 * 60 * 60)
#define SCAN_START_MARGIN_ENTRIES 64
#define PDM_PROBE_WINDOW 4

// Titles whose launches the journal cannot attribute, already written to the
// log once, so the same line is not put on the SD card on every heartbeat for
// the rest of the console's uptime.
static std::vector<u64> g_unattributedTitlesReported;
static const size_t UNATTRIBUTED_TITLES_REPORT_LIMIT = 32;

// True the first time it is asked about a title. Past the limit it stops
// remembering and starts saying true again — a full list means something is
// very wrong, and the log is how it would be found.
static bool firstReportForTitle(u64 titleId) {
    for (u64 reported : g_unattributedTitlesReported) {
        if (reported == titleId) return false;
    }

    if (g_unattributedTitlesReported.size() < UNATTRIBUTED_TITLES_REPORT_LIMIT) {
        g_unattributedTitlesReported.push_back(titleId);
    }
    return true;
}

static s64 daysFromCivil(s64 year, unsigned month, unsigned day) {
    year -= month <= 2;
    const s64 era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

static s64 parseServerWatermark(const std::string& value) {
    if (value.empty()) return 0;

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return 0;
    }

    return daysFromCivil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 + minute * 60 + second;
}

const char* playJournalEventTypeName(PlayJournalEventType type) {
    switch (type) {
        case PlayJournalEventType::Launch:
            return "launch";
        case PlayJournalEventType::Exit:
            return "exit";
        case PlayJournalEventType::InFocus:
            return "inFocus";
        case PlayJournalEventType::OutOfFocus:
            return "outOfFocus";
        case PlayJournalEventType::AccountLogin:
            return "accountLogin";
        case PlayJournalEventType::AccountLogout:
            return "accountLogout";
    }
    return "unknown";
}

static u64 playEventProgramId(const PdmPlayEvent& event) {
    return ((u64)event.event_data.applet.program_id[0] << 32) | (u64)event.event_data.applet.program_id[1];
}

static bool appletEventType(u8 raw, PlayJournalEventType* out) {
    switch (raw) {
        case PdmAppletEventType_Launch:
            *out = PlayJournalEventType::Launch;
            return true;
        case PdmAppletEventType_Exit:
        case PdmAppletEventType_Exit5:
        case PdmAppletEventType_Exit6:
            *out = PlayJournalEventType::Exit;
            return true;
        case PdmAppletEventType_InFocus:
            *out = PlayJournalEventType::InFocus;
            return true;
        case PdmAppletEventType_OutOfFocus:
        case PdmAppletEventType_OutOfFocus4:
            *out = PlayJournalEventType::OutOfFocus;
            return true;
        default:
            return false;
    }
}

// Where to begin reading, given what the server already has.
//
// The journal holds every launch the console has ever seen and only grows, and
// the sessions worth sending are always at its end. Walking to them from entry
// zero is what made a scan take thirteen seconds on a console with a few
// thousand entries: the cost is not the parsing but one IPC round trip to pdm —
// and through it to the internal storage — per thirty-two entries.
//
// The journal is chronological and QueryPlayEvent takes an arbitrary offset, so
// the tail can be found by bisection instead: about a dozen probes rather than
// hundreds of sequential reads. Returning 0 is always safe — it is the old
// behaviour — so every probe that cannot answer falls back to it.
static s32 findScanStart(s32 total, s64 watermarkEpoch) {
    // Nothing on the server yet: the oldest sessions are the ones owed, and the
    // scan stops on its own after maxSessions of them. Starting at zero is both
    // correct and cheap here.
    if (watermarkEpoch == 0) return 0;
    if (total <= PDM_READ_WINDOW) return 0;

    const s64 target = watermarkEpoch - SCAN_START_LOOKBACK_SEC;
    s32 low = 0;
    s32 high = total;  // Lowest index known to be newer than the target.

    while (low < high) {
        const s32 middle = low + (high - low) / 2;

        PdmPlayEvent probe[PDM_PROBE_WINDOW] = {};
        s32 count = 0;
        if (R_FAILED(pdmqryQueryPlayEvent(middle, probe, PDM_PROBE_WINDOW, &count)) || count <= 0) return 0;

        // Not every entry carries a user timestamp. Take the first that does:
        // timestamps do not decrease, so an answer read slightly past `middle`
        // can only move the result earlier, which is the safe direction.
        u64 timestamp = 0;
        for (s32 i = 0; i < count && timestamp == 0; i++) timestamp = probe[i].timestamp_user;
        if (timestamp == 0) return 0;

        if ((s64)timestamp + g_timeOffset > target) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    return low > SCAN_START_MARGIN_ENTRIES ? low - SCAN_START_MARGIN_ENTRIES : 0;
}

struct PendingSession {
    bool active = false;
    bool overflow = false;
    bool hasAccountLogin = false;
    s32 launchIndex = -1;
    u64 launchTimestamp = 0;
    u64 lastBoundaryTimestamp = 0;
    PlayJournalSession value;
};

static void startSession(PendingSession* session, u64 titleId, s32 entryIndex, const PdmPlayEvent& raw) {
    *session = PendingSession{};
    session->active = true;
    session->launchIndex = entryIndex;
    session->launchTimestamp = raw.timestamp_user;
    session->value.titleId = titleId;

    char title[513] = {};
    if (getTitleName(titleId, title, sizeof(title))) {
        session->value.title = title;
    } else {
        char fallback[17];
        snprintf(fallback, sizeof(fallback), "%016llX", (unsigned long long)titleId);
        session->value.title = fallback;
    }

    session->value.events.push_back(
        {PlayJournalEventType::Launch, raw.timestamp_user, raw.timestamp_steady, std::string()});
}

static void appendEvent(PendingSession* session, const PlayJournalEvent& event) {
    if (session->overflow) return;
    if (session->value.events.size() >= MAX_EVENTS_PER_SESSION) {
        session->overflow = true;
        return;
    }

    session->value.events.push_back(event);
    if (event.type == PlayJournalEventType::OutOfFocus || event.type == PlayJournalEventType::Exit) {
        session->lastBoundaryTimestamp = event.clockTimestamp;
    }
    if (event.type == PlayJournalEventType::AccountLogin && !event.account.empty()) {
        session->hasAccountLogin = true;
    }
}

static void finishSession(const IngestConfig& cfg, PendingSession* session, s64 watermark,
                          std::vector<PlayJournalSession>* out) {
    if (!session->active || session->overflow || session->lastBoundaryTimestamp == 0) {
        *session = PendingSession{};
        return;
    }

    const s64 correctedEnd = (s64)session->lastBoundaryTimestamp + g_timeOffset;
    if (correctedEnd <= watermark) {
        *session = PendingSession{};
        return;
    }

    if (!session->hasAccountLogin) {
        UserInfo user = {};
        if (getUserForPlayLaunch(session->value.titleId, session->launchIndex, session->launchTimestamp, &user)) {
            session->value.accountHint = resolveServerAccount(cfg, &user);
        }
    }

    // Nothing in the journal names an account for this launch. That is what a
    // homebrew port or a custom-id forwarder looks like from here: it never asks
    // for a profile, so it never opens one, so pdm has no account event to
    // record and no per-user statistics to fall back on either.
    //
    // The console's standing answer is used instead, when it has one: its only
    // profile, or the profile the owner named in defaultUid. That is not
    // guessing at whoever is at hand — it is a fact about the console in the
    // first case and the owner's own instruction in the second.
    if (!session->hasAccountLogin && session->value.accountHint.empty()) {
        AccountUid uid = {};
        UserInfo user = {};
        if ((getOnlyRegisteredUser(&uid) || getConfiguredDefaultUser(cfg.defaultUid.c_str(), &uid)) &&
            getUserInfo(&uid, &user)) {
            session->value.accountHint = resolveServerAccount(cfg, &user);

            // Said once per title. Unlike a drop these sessions do reach the
            // server and do advance the watermark, so a repeat means the server
            // refused them and the line is worth seeing again.
            if (firstReportForTitle(session->value.titleId)) {
                writeToLog("[Journal] %016llX names no account — filing its sessions under %s.",
                           (unsigned long long)session->value.titleId, session->value.accountHint.c_str());
            }
        }
    }

    // Still nobody. Sending it anyway is what stalled the whole backfill: the
    // server answers "session has no account", the watermark does not move, and
    // the next round offers the same session again.
    //
    // Dropping it is the documented trade in this module — a session filed under
    // whoever happens to be at hand is worse than a session that never arrives —
    // but a silent drop would be a hole in the record nobody could see, so each
    // one says which title and when.
    if (!session->hasAccountLogin && session->value.accountHint.empty()) {
        // Said once per title, not once per scan. A dropped session never
        // advances the watermark, so every later scan finds it again and would
        // report it again — forever, at one line per heartbeat.
        if (firstReportForTitle(session->value.titleId)) {
            writeToLog("[Journal] Dropping %016llX: the journal names no account for this launch, "
                       "and no defaultUid is set.",
                       (unsigned long long)session->value.titleId);
        }

        *session = PendingSession{};
        return;
    }

    out->push_back(session->value);
    *session = PendingSession{};
}

bool collectPendingPlaySessions(const IngestConfig& cfg, const std::string& watermark, size_t maxSessions,
                                std::vector<PlayJournalSession>* out) {
    if (out == NULL || maxSessions == 0) return false;
    out->clear();

    if (!openUserServices()) {
        writeToLog("[Journal] Could not open acc/pdm services.");
        return false;
    }

    s32 total = 0;
    s32 start = 0;
    s32 end = 0;
    Result rc = pdmqryGetAvailablePlayEventRange(&total, &start, &end);
    if (R_FAILED(rc) || total <= 0) {
        closeUserServices();
        return R_SUCCEEDED(rc);
    }

    const s64 watermarkEpoch = parseServerWatermark(watermark);
    PendingSession current;
    PdmPlayEvent events[PDM_READ_WINDOW] = {};
    // QueryPlayEvent uses a zero-based offset into the available event stream.
    // The range's start/end values are not valid offsets on all firmwares.
    writeTrace("      pdm: findScanStart (bisection)");
    s32 offset = findScanStart(total, watermarkEpoch);

    // Said once: the journal's size is the number that explains how long a scan
    // takes, and it was guesswork until it appeared here.
    static bool sizeAnnounced = false;
    if (!sizeAnnounced) {
        sizeAnnounced = true;
        writeToLog("[Journal] The play journal holds %d entries; this scan starts at %d.", total, offset);
    }

    while (out->size() < maxSessions && offset < total) {
        s32 count = 0;
        memset(events, 0, sizeof(events));
        rc = pdmqryQueryPlayEvent(offset, events, PDM_READ_WINDOW, &count);
        if (R_FAILED(rc) || count <= 0) break;

        for (s32 i = 0; i < count && out->size() < maxSessions; i++) {
            const s32 entryIndex = offset + i;
            const PdmPlayEvent& raw = events[i];

            if (raw.play_event_type == PdmPlayEventType_Applet) {
                if (raw.event_data.applet.applet_id != AppletId_application ||
                    raw.event_data.applet.log_policy != PdmPlayLogPolicy_All) {
                    continue;
                }

                PlayJournalEventType type;
                if (!appletEventType(raw.event_data.applet.event_type, &type)) continue;
                const u64 titleId = playEventProgramId(raw);

                if (type == PlayJournalEventType::Launch) {
                    // A new launch closes a crash-truncated predecessor at its
                    // last focus loss, exactly as NX Activity Log does.
                    finishSession(cfg, &current, watermarkEpoch, out);
                    if (out->size() >= maxSessions) break;
                    startSession(&current, titleId, entryIndex, raw);
                    continue;
                }

                if (!current.active || (titleId & ~0xFFFULL) != (current.value.titleId & ~0xFFFULL)) continue;
                appendEvent(&current, {type, raw.timestamp_user, raw.timestamp_steady, std::string()});

                if (type == PlayJournalEventType::Exit) finishSession(cfg, &current, watermarkEpoch, out);
            } else if (raw.play_event_type == PdmPlayEventType_Account && current.active &&
                       (raw.event_data.account.type == 0 || raw.event_data.account.type == 1)) {
                UserInfo user = {};
                std::string account;
                if (getUserForPlayEventIndex(entryIndex, &user)) account = resolveServerAccount(cfg, &user);

                const PlayJournalEventType type = raw.event_data.account.type == 0
                                                      ? PlayJournalEventType::AccountLogin
                                                      : PlayJournalEventType::AccountLogout;
                appendEvent(&current, {type, raw.timestamp_user, raw.timestamp_steady, account});
            }
        }

        offset += count;
        if (count < PDM_READ_WINDOW) break;
    }

    closeUserServices();

    if (R_FAILED(rc)) {
        writeToLog("[Journal] pdmqryQueryPlayEvent failed at %d (0x%08X).", offset, rc);
        out->clear();
        return false;
    }

    if (!out->empty()) {
        writeToLog("[Journal] Prepared %u closed session(s) newer than %s.", (unsigned)out->size(),
                   watermark.empty() ? "the beginning" : watermark.c_str());
    }
    return true;
}
