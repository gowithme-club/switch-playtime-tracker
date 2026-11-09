#pragma once

#include <string>

#include "config.hpp"
#include "play_journal.hpp"
#include "utilities.hpp"

/// POSTs /ingest/v1/heartbeat. `playing` is NULL when the console is awake but
/// nobody is in a game, which the server reads as "device on, live status
/// empty". `startedAtSec` is the corrected epoch second the current session
/// started, and `account` names whose session it is — both ignored when
/// `playing` is NULL.
///
/// On success `watermarkOut` receives the server's `sessionsWatermark` (empty
/// string when the server reports null). Returns false on any transport or
/// non-2xx failure — the caller just retries on the next cycle.
///
/// `titleNameOut`, when non-NULL, receives the name the server resolved for
/// `playing` (its `current[0].titleName`), or is cleared when it has none.
/// This console cannot read its own NACP (see AGENTS.md), so the server is
/// the only place a name can come from.
bool ingestHeartbeat(const IngestConfig& cfg, const AppInfo* playing, u64 startedAtSec, const std::string& account,
                     std::string* watermarkOut, std::string* titleNameOut = NULL);

/// One session the server newly stored, as it named and timed it.
struct IngestStoredSession {
    std::string titleName;  ///< server's name, or the bare title id
    int seconds = 0;        ///< the server's own duration
    int totalSeconds = 0;   ///< the person's lifetime in the game; 0 when the server did not say
    bool recent = false;    ///< ended within the last two days — worth a toast
};

/// What one play-sessions POST achieved, for the toast after a backfill.
struct IngestBatchResult {
    int accepted = 0;      ///< rows the server newly took; duplicates count 0
    int recent = 0;        ///< of those, how many ended within the last two days
    int totalSeconds = 0;  ///< summed over every accepted row
    /// How old a stored session may be and still get a toast. The first run
    /// drains the console's whole journal — years of launches, a few every
    /// minute — and a toast for each of those is noise, not news.
    static const int RECENT_SEC = 2 * 24 * 3600;
    /// The first few accepted rows, capped at `MAX_KEPT` so a long backfill
    /// does not grow the heap by the size of its own answer.
    static const size_t MAX_KEPT = 3;
    std::vector<IngestStoredSession> kept;
};

/// Sends complete raw PDM journal slices. The server owns account selection,
/// focus-time calculation, filtering and deduplication. `sessions` is consumed:
/// it is emptied and its memory freed once the body is built, before the
/// request goes out.
/// `resultOut`, when non-NULL, is filled from the server's answer; transport
/// failures leave it untouched.
bool ingestPlaySessions(const IngestConfig& cfg, std::vector<PlayJournalSession>* sessions,
                        std::string* watermarkOut, IngestBatchResult* resultOut = NULL);
