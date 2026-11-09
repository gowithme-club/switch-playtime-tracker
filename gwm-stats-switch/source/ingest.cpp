#include "ingest.hpp"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "http.hpp"
#include "logging.hpp"

// One POST. Returns true only on a 2xx, with the body in `response`.
static bool postJson(const IngestConfig& cfg, const char* path, const char* body, std::string* response) {
    return httpPost(cfg.baseUrl, path, cfg.token, body, response);
}

static void readWatermark(const std::string& response, std::string* watermarkOut) {
    if (watermarkOut == NULL) return;
    watermarkOut->clear();

    json_object* parsed = json_tokener_parse(response.c_str());
    if (parsed) {
        json_object* watermark = NULL;
        if (json_object_object_get_ex(parsed, "sessionsWatermark", &watermark) &&
            json_object_is_type(watermark, json_type_string)) {
            watermarkOut->assign(json_object_get_string(watermark));
        }
        json_object_put(parsed);
    }
}

// Copies a string field out of a json object, bounded: a toast has room for
// forty characters, and nothing else reads these names.
static void readBoundedString(json_object* obj, const char* key, std::string* out) {
    out->clear();
    json_object* value = NULL;
    if (json_object_object_get_ex(obj, key, &value) && json_object_is_type(value, json_type_string)) {
        const char* text = json_object_get_string(value);
        if (text != NULL) out->assign(text, strnlen(text, 120));
    }
}

bool ingestHeartbeat(const IngestConfig& cfg, const AppInfo* playing, u64 startedAtSec, const std::string& account,
                     std::string* watermarkOut, std::string* titleNameOut) {
    char sentAt[32];
    formatIso8601(getCorrectedNowSec(), sentAt, sizeof(sentAt));

    json_object* root = json_object_new_object();
    json_object_object_add(root, "sentAt", json_object_new_string(sentAt));

    json_object* current = json_object_new_array();

    if (playing != NULL) {
        char titleId[17];
        snprintf(titleId, sizeof(titleId), "%016llX", (unsigned long long)playing->tid);

        char startedAt[32];
        formatIso8601(startedAtSec, startedAt, sizeof(startedAt));

        json_object* entry = json_object_new_object();
        // Whose session this is. The console is shared, so this is the profile
        // that launched the game, not a per-console constant.
        json_object_object_add(entry, "account", json_object_new_string(account.c_str()));
        json_object_object_add(entry, "platform", json_object_new_string("switch"));
        json_object_object_add(entry, "titleId", json_object_new_string(titleId));
        // The name comes from the NACP and can be missing (control data absent
        // for some installs). The server accepts a heartbeat without it — the
        // title id alone still identifies the game.
        if (playing->title_name[0] != '\0') {
            json_object_object_add(entry, "titleName", json_object_new_string(playing->title_name));
        }
        json_object_object_add(entry, "startedAt", json_object_new_string(startedAt));
        json_object_array_add(current, entry);
    }

    json_object_object_add(root, "current", current);

    const char* body = json_object_to_json_string(root);
    std::string response;
    const bool ok = postJson(cfg, "/ingest/v1/heartbeat", body, &response);
    json_object_put(root);

    if (!ok) return false;

    // The response is the only back-channel the server has: it carries the
    // sessions watermark that will drive the play-record upload, and the name
    // it resolved for what we said we were running.
    readWatermark(response, watermarkOut);

    if (titleNameOut != NULL) {
        titleNameOut->clear();
        json_object* parsed = json_tokener_parse(response.c_str());
        if (parsed) {
            json_object* current = NULL;
            if (json_object_object_get_ex(parsed, "current", &current) &&
                json_object_is_type(current, json_type_array) && json_object_array_length(current) > 0) {
                json_object* first = json_object_array_get_idx(current, 0);
                if (first != NULL) readBoundedString(first, "titleName", titleNameOut);
            }
            json_object_put(parsed);
        }
    }

    return true;
}

// Appends `text` as a JSON string literal. Names are UTF-8 and pass through;
// only quotes, backslashes and control bytes need escaping.
static void appendJsonString(std::string* out, const std::string& text) {
    out->push_back('"');
    for (const char c : text) {
        const unsigned char byte = (unsigned char)c;
        if (c == '"' || c == '\\') {
            out->push_back('\\');
            out->push_back(c);
        } else if (byte < 0x20) {
            char escaped[8];
            snprintf(escaped, sizeof(escaped), "\\u%04X", byte);
            out->append(escaped);
        } else {
            out->push_back(c);
        }
    }
    out->push_back('"');
}

// The body is written straight into one string sized up front. A json-c tree
// of the same batch — an object with its own hash table per event, up to 128
// events a session — plus the printbuf it serializes into, doubling as it
// grows, held ~64 KB while curl was trying to allocate, and a long session
// (Super Mario Galaxy 2, 2026-09-23) failed with CURLE_OUT_OF_MEMORY on every
// attempt, reboot or not.
static std::string buildPlaySessionsBody(const std::vector<PlayJournalSession>& sessions) {
    size_t estimate = 64;
    for (const PlayJournalSession& session : sessions) {
        estimate += 96 + session.title.size() + session.accountHint.size();
        for (const PlayJournalEvent& event : session.events) estimate += 96 + event.account.size();
    }

    std::string body;
    body.reserve(estimate);
    char number[32];
    snprintf(number, sizeof(number), "%lld", (long long)g_timeOffset);
    body.append("{\"clockOffsetSeconds\":").append(number).append(",\"playSessions\":[");

    for (size_t i = 0; i < sessions.size(); i++) {
        const PlayJournalSession& session = sessions[i];
        if (i > 0) body.push_back(',');
        char titleId[17];
        snprintf(titleId, sizeof(titleId), "%016llX", (unsigned long long)session.titleId);
        body.append("{\"titleId\":\"").append(titleId).append("\",\"title\":");
        appendJsonString(&body, session.title);
        if (!session.accountHint.empty()) {
            body.append(",\"accountHint\":");
            appendJsonString(&body, session.accountHint);
        }
        body.append(",\"events\":[");
        for (size_t j = 0; j < session.events.size(); j++) {
            const PlayJournalEvent& event = session.events[j];
            if (j > 0) body.push_back(',');
            body.append("{\"type\":\"").append(playJournalEventTypeName(event.type)).append("\"");
            snprintf(number, sizeof(number), "%llu", (unsigned long long)event.clockTimestamp);
            body.append(",\"clockTimestamp\":").append(number);
            snprintf(number, sizeof(number), "%llu", (unsigned long long)event.steadyTimestamp);
            body.append(",\"steadyTimestamp\":").append(number);
            if (!event.account.empty()) {
                body.append(",\"account\":");
                appendJsonString(&body, event.account);
            }
            body.push_back('}');
        }
        body.append("]}");
    }

    body.append("]}");
    return body;
}

bool ingestPlaySessions(const IngestConfig& cfg, std::vector<PlayJournalSession>* sessions,
                        std::string* watermarkOut, IngestBatchResult* resultOut) {
    if (resultOut != NULL) *resultOut = IngestBatchResult();
    if (sessions->empty()) return true;

    const std::string body = buildPlaySessionsBody(*sessions);
    // The body is all the request needs; the sessions go back to the heap
    // before curl asks it for anything.
    std::vector<PlayJournalSession>().swap(*sessions);

    std::string response;
    const bool ok = postJson(cfg, "/ingest/v1/switch/play-sessions", body.c_str(), &response);

    if (!ok) return false;
    readWatermark(response, watermarkOut);

    json_object* parsed = json_tokener_parse(response.c_str());
    if (parsed) {
        json_object* accepted = NULL;
        json_object* duplicates = NULL;
        json_object* rejected = NULL;
        const int acceptedCount = json_object_object_get_ex(parsed, "accepted", &accepted)
                                      ? json_object_get_int(accepted)
                                      : 0;
        if (resultOut != NULL) {
            resultOut->accepted = acceptedCount;
            resultOut->recent = 0;
            resultOut->totalSeconds = 0;
            resultOut->kept.clear();
            const u64 nowSec = getCorrectedNowSec();
            // `sessions` lists what was newly stored, already named and timed
            // by the server. Only the first few are kept — see MAX_KEPT.
            json_object* stored = NULL;
            if (json_object_object_get_ex(parsed, "sessions", &stored) && json_object_is_type(stored, json_type_array)) {
                const size_t count = json_object_array_length(stored);
                for (size_t i = 0; i < count; i++) {
                    json_object* row = json_object_array_get_idx(stored, i);
                    if (row == NULL) continue;
                    json_object* seconds = NULL;
                    const int rowSeconds = json_object_object_get_ex(row, "seconds", &seconds)
                                               ? json_object_get_int(seconds)
                                               : 0;
                    resultOut->totalSeconds += rowSeconds;
                    std::string endedAt;
                    readBoundedString(row, "endedAt", &endedAt);
                    const u64 ended = parseIso8601(endedAt.c_str());
                    const bool recent = ended != 0 && ended + IngestBatchResult::RECENT_SEC >= nowSec;
                    if (recent) resultOut->recent++;
                    if (resultOut->kept.size() >= IngestBatchResult::MAX_KEPT) continue;
                    IngestStoredSession entry;
                    entry.seconds = rowSeconds;
                    entry.recent = recent;
                    json_object* total = NULL;
                    if (json_object_object_get_ex(row, "totalSeconds", &total) && json_object_is_type(total, json_type_int)) {
                        entry.totalSeconds = json_object_get_int(total);
                    }
                    readBoundedString(row, "titleName", &entry.titleName);
                    if (entry.titleName.empty()) readBoundedString(row, "titleId", &entry.titleName);
                    resultOut->kept.push_back(entry);
                }
            }
        }
        const int duplicateCount = json_object_object_get_ex(parsed, "duplicates", &duplicates)
                                       ? json_object_get_int(duplicates)
                                       : 0;
        const int rejectedCount = json_object_object_get_ex(parsed, "rejected", &rejected) &&
                                          json_object_is_type(rejected, json_type_array)
                                      ? (int)json_object_array_length(rejected)
                                      : 0;
        writeToLog("[Journal] Server accepted=%d duplicate=%d rejected=%d.", acceptedCount, duplicateCount,
                   rejectedCount);

        // The rejected array says why, and throwing it away left the module
        // resending the same two sessions every cycle with no way to know what
        // was wrong with them. One entry is enough to act on; the rest of a
        // batch fails for the same reason in practice.
        if (rejectedCount > 0) {
            json_object* first = json_object_array_get_idx(rejected, 0);
            if (first != NULL) {
                const char* text = json_object_to_json_string_ext(first, JSON_C_TO_STRING_PLAIN);
                if (text != NULL) writeToLog("[Journal] First rejection: %.200s", text);
            }
        }
        json_object_put(parsed);
    }

    return true;
}
