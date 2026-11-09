#pragma once

#include <switch.h>

#include <string>
#include <vector>

#include "config.hpp"

enum class PlayJournalEventType {
    Launch,
    Exit,
    InFocus,
    OutOfFocus,
    AccountLogin,
    AccountLogout,
};

struct PlayJournalEvent {
    PlayJournalEventType type;
    u64 clockTimestamp;
    u64 steadyTimestamp;
    std::string account;
};

struct PlayJournalSession {
    u64 titleId;
    std::string title;
    std::string accountHint;
    std::vector<PlayJournalEvent> events;
};

/// Reads complete launch-to-close slices from Horizon's PDM journal. Only
/// sessions newer than the server watermark are returned, oldest first. The
/// caller closes pdm/acc before doing network I/O.
bool collectPendingPlaySessions(const IngestConfig& cfg, const std::string& watermark, size_t maxSessions,
                                std::vector<PlayJournalSession>* out);

const char* playJournalEventTypeName(PlayJournalEventType type);
