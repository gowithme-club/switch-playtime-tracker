#pragma once

#include <string>
#include <vector>

#include "utilities.hpp"

struct AccountMapping {
    std::string uid;
    std::string account;
};

/// Where the module reports unless the config says otherwise. This is the
/// project's own client, so the address is ours to know — a person installing
/// it only has a token to supply.
#define GWM_DEFAULT_INGEST_URL "http://gowithme.club"

/// How often a heartbeat goes out, in seconds. The server forgets live status
/// five minutes after the last one, so this is a balance we own, not a setting.
#define GWM_HEARTBEAT_SECONDS 60

/// Everything the module needs, read from sdmc:/config/gwm-stats-switch/config.json.
/// See docs/DEVELOPMENT.ru.md for the file format.
struct IngestConfig {
    bool valid = false;            ///< false when the file is missing or unusable
    std::string baseUrl;           ///< GWM_DEFAULT_INGEST_URL unless overridden, no trailing slash
    std::string token;             ///< device token, sent as `Authorization: Bearer`
    std::vector<AccountMapping> accounts;  ///< stable UID -> server account aliases
    std::string defaultUid;        ///< canonical UID to file unattributed launches under, empty = none
    bool ultrahandNotify = true;   ///< Ultrahand `.notify` toasts; on unless switched off
};

/// Loads the config if the file changed since the last call (or on first call).
/// Returns true when `cfg` was replaced. A missing or invalid file leaves
/// `cfg.valid` false and the module idles instead of failing.
bool loadConfigIfChanged(IngestConfig* cfg);

/// Stable configured alias first, local nickname second, UID third, and the
/// bare "switch" when no profile is known at all. This mapping is transport
/// identity, not session inference.
std::string resolveServerAccount(const IngestConfig& cfg, const UserInfo* user);
