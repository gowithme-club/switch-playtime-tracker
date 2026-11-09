#include "notify.hpp"

#include <json-c/json.h>

#include "logging.hpp"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

// Ultrahand polls this folder ~every 300 ms. The overlay process has to be
// alive (nx-ovlloader + ovlmenu); a write while it is down just sits on the
// card until the next launch, or gets deleted on ovlmenu boot. That is the
// user's problem, not ours — we do not queue, retry, or log a miss.
static const char* NOTIFY_DIR = "sdmc:/config/ultrahand/notifications";

void notifyUltrahand(const IngestConfig& cfg, const char* text) {
    if (!cfg.ultrahandNotify || text == NULL || text[0] == '\0') return;

    // Only where something will read them. The toasts are on by default, and
    // a console without a reader must not collect a folder of files nobody
    // opens. /config/ultrahand alone proves nothing: Uberhand — the fork that
    // Kefir and 4IFIR ship — keeps its files there too and has no notification
    // API at all. The notifications folder is made by Ultrahand itself (2.1+),
    // and by nothing else, so its presence is the reader's presence. Never
    // created here.
    struct stat st;
    if (stat(NOTIFY_DIR, &st) != 0) return;

    json_object* root = json_object_new_object();
    if (root == NULL) return;
    json_object_object_add(root, "title", json_object_new_string("gowithme.club"));
    json_object_object_add(root, "text", json_object_new_string(text));
    json_object_object_add(root, "duration", json_object_new_int(4000));
    json_object_object_add(root, "alignment", json_object_new_string("left"));
    json_object_object_add(root, "show_time", json_object_new_string("true"));

    const char* body = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (body == NULL) {
        json_object_put(root);
        return;
    }

    // Tick is unique enough across writes; Ultrahand keys on the filename.
    const unsigned long long id = (unsigned long long)armGetSystemTick();
    char tmp[160];
    char path[160];
    snprintf(tmp, sizeof(tmp), "%s/gwmstats-%llu.notify.tmp", NOTIFY_DIR, id);
    snprintf(path, sizeof(path), "%s/gwmstats-%llu.notify", NOTIFY_DIR, id);

    // Logged whatever happens next: the overlay consumes the file within a
    // second and may delete it unread if its API notifications are off, so
    // the log is the only place the text survives to be checked against what
    // the screen showed.
    // The log is one entry per line, so a two-line toast is logged with its
    // break shown as " | ".
    char logged[160];
    snprintf(logged, sizeof(logged), "%s", text);
    for (char* c = logged; *c != '\0'; c++) {
        if (*c == '\n') *c = '|';
    }

    FILE* f = fopen(tmp, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
        rename(tmp, path);
        writeToLog("[Notify] %s", logged);
    } else {
        writeToLog("[Notify] Could not write %s: %s", path, logged);
    }

    json_object_put(root);
}
