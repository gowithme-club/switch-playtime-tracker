#include "config.hpp"

#include <json-c/json.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "logging.hpp"
#include "paths.hpp"

// The new folder wins when both exist; the legacy one keeps an already
// installed card working. The chosen path is remembered so a config that
// appears in the new folder later is noticed as a change.
static const char* const CONFIG_PATHS[] = {
    GWM_CONFIG_DIR "/config.json",
    GWM_LEGACY_CONFIG_DIR "/config.json",
};

static time_t g_lastMtime = 0;
static bool g_everLoaded = false;
static const char* g_lastPath = NULL;

static const char* findConfig(struct stat* st) {
    for (const char* path : CONFIG_PATHS) {
        if (stat(path, st) == 0) return path;
    }
    return NULL;
}

static std::string jsonString(json_object* root, const char* key, const char* fallback) {
    json_object* value = NULL;
    if (json_object_object_get_ex(root, key, &value) && json_object_is_type(value, json_type_string)) {
        return std::string(json_object_get_string(value));
    }
    return std::string(fallback);
}

static bool jsonBool(json_object* root, const char* key, bool fallback) {
    json_object* value = NULL;
    if (!json_object_object_get_ex(root, key, &value)) return fallback;
    if (json_object_is_type(value, json_type_boolean)) return json_object_get_boolean(value);
    if (json_object_is_type(value, json_type_int)) return json_object_get_int(value) != 0;
    if (json_object_is_type(value, json_type_string)) {
        const char* s = json_object_get_string(value);
        return s != NULL && (strcmp(s, "true") == 0 || strcmp(s, "1") == 0);
    }
    return fallback;
}

static void trimTrailingSlash(std::string* url) {
    while (!url->empty() && (*url)[url->size() - 1] == '/') {
        url->erase(url->size() - 1);
    }
}

static std::string canonicalUid(const char* raw) {
    std::string result;
    if (raw == NULL) return result;

    for (const char* p = raw; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isxdigit(c)) result.push_back((char)toupper(c));
    }

    return result.size() == 32 ? result : std::string();
}

static bool validServerAccount(const std::string& account) {
    return !account.empty() && account.size() <= 64 && account.find(':') == std::string::npos;
}

static void loadAccountMappings(json_object* root, std::vector<AccountMapping>* out) {
    json_object* accounts = NULL;
    if (!json_object_object_get_ex(root, "accounts", &accounts) ||
        !json_object_is_type(accounts, json_type_object)) {
        return;
    }

    json_object_object_foreach(accounts, rawUid, value) {
        if (!json_object_is_type(value, json_type_string)) continue;
        const std::string uid = canonicalUid(rawUid);
        const std::string account = json_object_get_string(value);
        if (!uid.empty() && validServerAccount(account)) out->push_back(AccountMapping{uid, account});
    }
}

bool loadConfigIfChanged(IngestConfig* cfg) {
    struct stat st;
    const char* configPath = findConfig(&st);

    if (configPath == NULL) {
        if (!g_everLoaded || cfg->valid) {
            writeToLog("[Config] No config at %s — idling until one appears.", CONFIG_PATHS[0]);
            *cfg = IngestConfig();
            g_everLoaded = true;
            g_lastMtime = 0;
            g_lastPath = NULL;
            return true;
        }
        return false;
    }

    if (g_everLoaded && configPath == g_lastPath && st.st_mtime == g_lastMtime) {
        return false;
    }

    g_lastMtime = st.st_mtime;
    g_lastPath = configPath;
    g_everLoaded = true;

    std::ifstream file(configPath);
    if (!file.is_open()) {
        writeToLog("[Config] Could not open %s", configPath);
        *cfg = IngestConfig();
        return true;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    json_object* root = json_tokener_parse(buffer.str().c_str());
    if (!root) {
        writeToLog("[Config] %s is not valid JSON — idling.", configPath);
        *cfg = IngestConfig();
        return true;
    }

    IngestConfig next;
    // The address is ours; the key stays as an override for a server run at
    // home, which is how the module is developed.
    next.baseUrl = jsonString(root, "ingestUrl", GWM_DEFAULT_INGEST_URL);
    next.token = jsonString(root, "token", "");
    loadAccountMappings(root, &next.accounts);
    const std::string rawDefaultUid = jsonString(root, "defaultUid", "");
    next.defaultUid = canonicalUid(rawDefaultUid.c_str());
    next.ultrahandNotify = jsonBool(root, "ultrahandNotify", true);
    json_object_put(root);

    // Non-fatal: a bad defaultUid costs the fallback, not the module. Said once
    // here rather than on every launch it fails to name.
    if (!rawDefaultUid.empty() && next.defaultUid.empty()) {
        writeToLog("[Config] defaultUid is not a 32-digit profile UID — ignoring it.");
    }

    trimTrailingSlash(&next.baseUrl);

    // There is no TLS in this build — it was dropped so the console keeps enough
    // system memory to open an applet, see docs/DEVELOPMENT.ru.md. An https:// URL has to be
    // refused here rather than in the transport: accepted, it would log as
    // loaded, report a healthy config, and then fail every single POST with a
    // line about the scheme far away from the file that chose it.
    if (next.baseUrl.compare(0, 8, "https://") == 0) {
        writeToLog("[Config] ingestUrl is https:// but this module has no TLS — use http:// — idling.");
        *cfg = IngestConfig();
        return true;
    }

    if (next.baseUrl.compare(0, 7, "http://") != 0) {
        writeToLog("[Config] ingestUrl must start with http:// — idling.");
        *cfg = IngestConfig();
        return true;
    }

    if (next.token.empty()) {
        writeToLog("[Config] token is empty — idling.");
        *cfg = IngestConfig();
        return true;
    }

    next.valid = true;
    *cfg = next;

    writeToLog("[Config] Loaded: url=%s mappedAccounts=%u defaultUid=%s ultrahandNotify=%s",
               next.baseUrl.c_str(), (unsigned)next.accounts.size(),
               next.defaultUid.empty() ? "-" : next.defaultUid.c_str(),
               next.ultrahandNotify ? "on" : "off");
    return true;
}

std::string resolveServerAccount(const IngestConfig& cfg, const UserInfo* user) {
    if (user != NULL && accountUidIsValid(&user->uid)) {
        for (const AccountMapping& mapping : cfg.accounts) {
            if (mapping.uid == user->uid_string) return mapping.account;
        }

        const std::string nickname(user->nickname);
        if (validServerAccount(nickname)) return nickname;
        return std::string("uid-") + user->uid_string;
    }

    return std::string("switch");
}
