#include "utilities.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>

#include <vector>

#include "logging.hpp"

void waitForNetworkReady() {
    writeToLog("[Network] Waiting for the network to come up...");

    while (true) {
        NifmInternetConnectionType connectionType;
        u32 wifiStrength;
        NifmInternetConnectionStatus connectionStatus;

        Result rc = nifmGetInternetConnectionStatus(&connectionType, &wifiStrength, &connectionStatus);

        if (R_SUCCEEDED(rc) && connectionStatus == NifmInternetConnectionStatus_Connected) {
            break;
        }

        svcSleepThread(1 * 1000 * 1000 * 1000ULL);
    }

    writeToLog("[Network] Network is up.");
}

Result get_app_info(AppInfo* info) {
    writeTrace("  pm: pmdmntGetApplicationProcessId");
    Result rc = pmdmntGetApplicationProcessId(&info->pid);
    if (R_FAILED(rc)) return rc;

    writeTrace("  pm: pmdmntGetProgramId");
    rc = pmdmntGetProgramId(&info->tid, info->pid);
    if (R_FAILED(rc)) return rc;

    writeTrace("  ns: nsGetApplicationControlData");
    return getTitleName(info->tid, info->title_name, sizeof(info->title_name))
               ? 0
               : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

// This console will not answer a control-data read smaller than the whole
// NsApplicationControlData. At 0x4000 — the nacp alone, without the 0x20000
// icon behind it — it returns 0x5010 for every title, installed or running, so
// nothing here can produce a name and games reach the server as bare IDs.
//
// The full 144 KB read does work; it was tried, and it named twenty titles on
// the first journal scan. It is not used, because holding a heap large enough
// for it made the browser applet hang the console — see INNER_HEAP_SIZE. Names
// have to come from the server side, where a title id costs nothing to resolve.
//
// So the small read stays, knowing it will fail, purely so that a future
// firmware or a different console gets a name if it can. The latch below is
// what keeps that from being expensive: a journal scan asks for a name once per
// session — 793 in a pass — and without it each scan wrote 793 identical
// failure lines to the SD card.
static int g_controlDataFailures = 0;
static const int CONTROL_DATA_FAILURE_LIMIT = 3;

// One 144 KB read per session in a journal scan would be 793 of them for a
// handful of distinct games. Names never change under us, so each title is
// asked for once and remembered.
// A NacpLanguageEntry name is a fixed 0x200-byte field, not a string. When the
// entry is unset — or the control data is not laid out the way the read assumed
// — those bytes are whatever happened to be there, and nothing about the call
// says so: it succeeds. On this console 01007EF00011E000 answers with 512 bytes
// of binary, which reached the log and would have reached the server as the
// game's name.
//
// A name has to end inside its own field, carry no control bytes, and be valid
// UTF-8 — real names here include ™, ® and Cyrillic ("Fortnite на Nintendo
// Switch"), so this cannot just demand ASCII.
static bool isPlausibleTitleName(const char* field, size_t fieldSize) {
    const size_t length = strnlen(field, fieldSize);
    if (length == 0 || length == fieldSize) return false;

    const unsigned char* bytes = (const unsigned char*)field;
    for (size_t i = 0; i < length;) {
        const unsigned char lead = bytes[i];
        size_t continuations;

        if (lead < 0x20) return false;
        else if (lead < 0x80) continuations = 0;
        else if ((lead & 0xE0) == 0xC0) continuations = 1;
        else if ((lead & 0xF0) == 0xE0) continuations = 2;
        else if ((lead & 0xF8) == 0xF0) continuations = 3;
        else return false;  // stray continuation byte, or a form UTF-8 does not have

        if (i + continuations >= length) return false;  // sequence runs off the end
        for (size_t k = 1; k <= continuations; k++) {
            if ((bytes[i + k] & 0xC0) != 0x80) return false;
        }
        i += continuations + 1;
    }

    return true;
}

struct TitleNameCacheEntry {
    u64 titleId;
    char name[513];
};
static std::vector<TitleNameCacheEntry> g_titleNameCache;
static const size_t TITLE_NAME_CACHE_LIMIT = 64;

bool getTitleName(u64 titleId, char* out, size_t outSize) {
    if (out == NULL || outSize == 0 || titleId == 0) return false;
    out[0] = '\0';

    for (const TitleNameCacheEntry& entry : g_titleNameCache) {
        if (entry.titleId != titleId) continue;
        if (entry.name[0] == '\0') return false;  // asked before, no name to be had
        strncpy(out, entry.name, outSize - 1);
        out[outSize - 1] = '\0';
        return true;
    }

    if (g_controlDataFailures >= CONTROL_DATA_FAILURE_LIMIT) return false;

    NacpStruct* controlData = (NacpStruct*)malloc(sizeof(NacpStruct));
    if (controlData == NULL) {
        writeToLog("[Name] No heap for a %u KB control-data read.",
                   (unsigned)(sizeof(NacpStruct) / 1024));
        return false;
    }

    memset(controlData, 0, sizeof(NacpStruct));
    u64 controlDataSize = sizeof(NacpStruct);
    NacpLanguageEntry* languageEntry = NULL;
    bool named = false;

    const Result controlRc =
        nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                    (NsApplicationControlData*)controlData, controlDataSize, &controlDataSize);

    if (R_FAILED(controlRc)) {
        // Said once, not once per title: the first refusal is the diagnosis and
        // the eight hundred after it are noise that costs SD writes.
        g_controlDataFailures++;
        if (g_controlDataFailures == 1) {
            writeToLog("[Name] nsGetApplicationControlData(%u KB) failed: 0x%08X — titles will be "
                       "reported by id, to be named server-side.",
                       (unsigned)(sizeof(NacpStruct) / 1024), controlRc);
        } else if (g_controlDataFailures == CONTROL_DATA_FAILURE_LIMIT) {
            writeToLog("[Name] Giving up on title names for this run after %d refusals.",
                       CONTROL_DATA_FAILURE_LIMIT);
        }
    } else {
        g_controlDataFailures = 0;

        const Result languageRc = nacpGetLanguageEntry(controlData, &languageEntry);
        if (R_FAILED(languageRc) || languageEntry == NULL) {
            writeToLog("[Name] nacpGetLanguageEntry failed: 0x%08X (read %llu bytes)", languageRc,
                       (unsigned long long)controlDataSize);
        } else if (!isPlausibleTitleName(languageEntry->name, sizeof(languageEntry->name))) {
            writeToLog("[Name] %016llX answered with %u bytes that are not a name; ignoring it.",
                       (unsigned long long)titleId,
                       (unsigned)strnlen(languageEntry->name, sizeof(languageEntry->name)));
        } else {
            strncpy(out, languageEntry->name, outSize - 1);
            out[outSize - 1] = '\0';
            named = true;
        }
    }

    free(controlData);

    // Remembered either way. A title the console cannot name is worth
    // remembering precisely so it is not asked for 793 times.
    if (g_titleNameCache.size() < TITLE_NAME_CACHE_LIMIT) {
        TitleNameCacheEntry entry = {};
        entry.titleId = titleId;
        if (named) {
            strncpy(entry.name, out, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
        }
        g_titleNameCache.push_back(entry);
    }

    if (named) writeToLog("[Name] %016llX is \"%s\".", (unsigned long long)titleId, out);
    return named;
}

bool getCurrentUserUid(AccountUid* out) {
    if (out == NULL) return false;

    AccountUid uid = {};
    writeTrace("    acc: accountGetLastOpenedUser");
    if (R_FAILED(accountGetLastOpenedUser(&uid)) || !accountUidIsValid(&uid)) return false;

    *out = uid;
    return true;
}

bool getUserName(const AccountUid* uid, char* out, size_t outSize) {
    if (uid == NULL || out == NULL || outSize == 0 || !accountUidIsValid(uid)) return false;

    AccountProfile profile;
    if (R_FAILED(accountGetProfile(&profile, *uid))) return false;

    AccountProfileBase base = {};
    const Result rc = accountProfileGet(&profile, NULL, &base);
    accountProfileClose(&profile);
    if (R_FAILED(rc)) return false;

    // nickname is a fixed 0x20 field and is not guaranteed to be terminated.
    char nickname[sizeof(base.nickname) + 1];
    memcpy(nickname, base.nickname, sizeof(base.nickname));
    nickname[sizeof(base.nickname)] = '\0';

    if (nickname[0] == '\0') return false;

    strncpy(out, nickname, outSize - 1);
    out[outSize - 1] = '\0';
    return true;
}

static void formatAccountUid(const AccountUid& uid, char* out, size_t outSize) {
    snprintf(out, outSize, "%016llX%016llX", (unsigned long long)uid.uid[1],
             (unsigned long long)uid.uid[0]);
}

bool getUserInfo(const AccountUid* uid, UserInfo* out) {
    if (uid == NULL || out == NULL || !accountUidIsValid(uid)) return false;

    memset(out, 0, sizeof(*out));
    out->uid = *uid;
    formatAccountUid(*uid, out->uid_string, sizeof(out->uid_string));
    getUserName(uid, out->nickname, sizeof(out->nickname));
    return true;
}

bool getUserForPlayEventIndex(s32 entryIndex, UserInfo* out) {
    if (out == NULL || entryIndex < 0) return false;

    PdmAccountEvent events[8] = {};
    s32 count = 0;
    if (R_FAILED(pdmqryQueryAccountEvent(entryIndex, events, 8, &count))) return false;

    for (s32 i = 0; i < count; i++) {
        if ((s32)events[i].entry_index != entryIndex || !accountUidIsValid(&events[i].uid)) continue;
        return getUserInfo(&events[i].uid, out);
    }

    return false;
}

bool getUserForPlayLaunch(u64 titleId, s32 launchIndex, u64 launchTimestamp, UserInfo* out) {
    if (out == NULL || titleId == 0 || launchIndex < 0) return false;

    const u64 applicationId = titleId & ~0xFFFULL;
    s32 total = 0;
    s32 start = 0;
    s32 end = 0;
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end))) return false;

    PdmAccountEvent accountEvents[64] = {};
    s32 queryStart = launchIndex - 24;
    if (queryStart < start) queryStart = start;
    s32 accountCount = 0;

    if (R_SUCCEEDED(pdmqryQueryAccountEvent(queryStart, accountEvents, 64, &accountCount))) {
        s32 best = -1;
        s32 bestIndexDistance = 0x7FFFFFFF;
        u64 bestTimeDistance = ~0ULL;

        for (s32 i = 0; i < accountCount; i++) {
            const PdmAccountEvent& event = accountEvents[i];
            if (!accountUidIsValid(&event.uid) || (event.program_id & ~0xFFFULL) != applicationId) continue;

            const s32 eventIndex = (s32)event.entry_index;
            const s32 indexDistance = eventIndex >= launchIndex ? eventIndex - launchIndex : launchIndex - eventIndex;
            if (indexDistance > 24) continue;

            const u64 eventTimestamp = event.timestamp_network != 0 ? event.timestamp_network : event.timestamp_user;
            const u64 timeDistance = eventTimestamp >= launchTimestamp ? eventTimestamp - launchTimestamp
                                                                        : launchTimestamp - eventTimestamp;
            if (launchTimestamp != 0 && eventTimestamp != 0 && timeDistance > 30) continue;

            if (best < 0 || timeDistance < bestTimeDistance ||
                (timeDistance == bestTimeDistance && indexDistance < bestIndexDistance)) {
                best = i;
                bestIndexDistance = indexDistance;
                bestTimeDistance = timeDistance;
            }
        }

        if (best >= 0) return getUserInfo(&accountEvents[best].uid, out);
    }

    AccountUid users[ACC_USER_LIST_SIZE] = {};
    s32 userCount = 0;
    if (R_FAILED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &userCount))) return false;

    s32 bestUser = -1;
    s32 bestIndexDistance = 0x7FFFFFFF;
    u64 bestTimeDistance = ~0ULL;

    for (s32 i = 0; i < userCount; i++) {
        PdmPlayStatistics stats = {};
        if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(applicationId, users[i], true, &stats))) {
            continue;
        }

        const s32 statsIndex = (s32)stats.last_entry_index;
        const s32 indexDistance = statsIndex >= launchIndex ? statsIndex - launchIndex : launchIndex - statsIndex;
        if (indexDistance > 24) continue;

        const u64 statsTimestamp = stats.last_timestamp_network != 0 ? stats.last_timestamp_network
                                                                      : stats.last_timestamp_user;
        const u64 timeDistance = statsTimestamp >= launchTimestamp ? statsTimestamp - launchTimestamp
                                                                    : launchTimestamp - statsTimestamp;
        if (launchTimestamp != 0 && statsTimestamp != 0 && timeDistance > 30) continue;

        if (bestUser < 0 || timeDistance < bestTimeDistance ||
            (timeDistance == bestTimeDistance && indexDistance < bestIndexDistance)) {
            bestUser = i;
            bestIndexDistance = indexDistance;
            bestTimeDistance = timeDistance;
        }
    }

    return bestUser >= 0 && getUserInfo(&users[bestUser], out);
}

static bool g_userServicesOpen = false;

bool openUserServices() {
    if (g_userServicesOpen) return true;

    // sm hands out service handles and is not needed afterwards, so it is
    // opened and dropped around the two that are.
    writeTrace("  sm: smInitialize");
    if (R_FAILED(smInitialize())) return false;

    writeTrace("  acc: accountInitialize");
    Result rc = accountInitialize(AccountServiceType_System);
    if (R_SUCCEEDED(rc)) {
        writeTrace("  pdm: pdmqryInitialize");
        rc = pdmqryInitialize();
        if (R_FAILED(rc)) accountExit();
    }

    writeTrace("  sm: smExit");
    smExit();

    g_userServicesOpen = R_SUCCEEDED(rc);
    return g_userServicesOpen;
}

void closeUserServices() {
    if (!g_userServicesOpen) return;

    pdmqryExit();
    accountExit();
    g_userServicesOpen = false;
}

// ListOpenUsers, acc cmd 3. libnx wraps ListAllUsers (cmd 2) and not this one,
// though this is the one that says who is playing rather than who exists.
static Result accountListOpenUsers(AccountUid* uids, s32 max_uids, s32* total) {
    return serviceDispatchOut(accountGetServiceSession(), 3, *total,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{uids, (size_t)max_uids * sizeof(AccountUid)}}, );
}

// Every profile registered on the console, open or not, in creation order.
static bool listRegisteredUsers(AccountUid* users, s32* count) {
    *count = 0;
    writeTrace("    acc: ListAllUsers");
    return R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, count)) && *count > 0;
}

bool getOnlyRegisteredUser(AccountUid* out) {
    if (out == NULL) return false;

    AccountUid users[ACC_USER_LIST_SIZE] = {};
    s32 count = 0;
    if (!listRegisteredUsers(users, &count)) return false;
    // Two profiles and nothing naming one is a guess; one profile is a fact.
    if (count != 1 || !accountUidIsValid(&users[0])) return false;

    *out = users[0];
    return true;
}

bool getConfiguredDefaultUser(const char* canonicalUid, AccountUid* out) {
    if (out == NULL || canonicalUid == NULL || canonicalUid[0] == '\0') return false;

    AccountUid users[ACC_USER_LIST_SIZE] = {};
    s32 count = 0;
    if (!listRegisteredUsers(users, &count)) return false;

    // Matched against the console rather than parsed and trusted: a typo in the
    // config must not invent a profile the server would then file sessions
    // under. No match means no answer, which is the same as no setting.
    for (s32 i = 0; i < count; i++) {
        char formatted[33];
        formatAccountUid(users[i], formatted, sizeof(formatted));
        if (strcmp(formatted, canonicalUid) != 0) continue;
        if (!accountUidIsValid(&users[i])) return false;

        *out = users[i];
        return true;
    }

    return false;
}

bool getSingleOpenUser(AccountUid* out) {
    if (out == NULL) return false;

    AccountUid uids[ACC_USER_LIST_SIZE] = {};
    s32 total = 0;

    writeTrace("    acc: ListOpenUsers (raw dispatch)");
    if (R_FAILED(accountListOpenUsers(uids, ACC_USER_LIST_SIZE, &total))) return false;
    // Two open users cannot be told apart from here, and none means the game
    // has not opened one yet. Only a single answer is an answer.
    if (total != 1 || !accountUidIsValid(&uids[0])) return false;

    *out = uids[0];
    return true;
}

bool getUserForTitle(u64 titleId, u64 sinceRawSec, AccountUid* out) {
    if (out == NULL || titleId == 0) return false;

    s32 total = 0;
    s32 start = 0;
    s32 end = 0;

    writeTrace("    pdm: pdmqryGetAvailablePlayEventRange");
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end)) || total <= 0) return false;

    const s32 WINDOW = 32;
    const s32 MAX_BACK = 256;
    PdmAccountEvent events[WINDOW] = {};

    for (s32 back = 0; back < MAX_BACK; back += WINDOW) {
        s32 index = end - back - WINDOW;
        if (index < start) index = start;

        s32 count = 0;
        memset(events, 0, sizeof(events));
        writeTrace("    pdm: pdmqryQueryAccountEvent");
        if (R_FAILED(pdmqryQueryAccountEvent(index, events, WINDOW, &count)) || count <= 0) return false;
        if (count > WINDOW) count = WINDOW;

        for (s32 i = count - 1; i >= 0; i--) {
            if (events[i].program_id != titleId) continue;
            if (events[i].timestamp_user < sinceRawSec) return false;
            if (!accountUidIsValid(&events[i].uid)) continue;
            *out = events[i].uid;
            return true;
        }

        if (index == start) break;
    }

    return false;
}

void describeUserSources(u64 titleId, u64 sinceRawSec, char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return;

    char name[0x21];
    AccountUid uid = {};
    size_t used = 0;

    used += snprintf(out + used, outSize - used, "pdm=");
    if (getUserForTitle(titleId, sinceRawSec, &uid) && getUserName(&uid, name, sizeof(name))) {
        used += snprintf(out + used, outSize - used, "%s", name);
    } else {
        used += snprintf(out + used, outSize - used, "-");
    }

    used += snprintf(out + used, outSize - used, " open=");
    if (getSingleOpenUser(&uid) && getUserName(&uid, name, sizeof(name))) {
        used += snprintf(out + used, outSize - used, "%s", name);
    } else {
        used += snprintf(out + used, outSize - used, "-");
    }

    used += snprintf(out + used, outSize - used, " last=");
    if (getCurrentUserUid(&uid) && getUserName(&uid, name, sizeof(name))) {
        snprintf(out + used, outSize - used, "%s", name);
    } else {
        snprintf(out + used, outSize - used, "-");
    }
}

// psc id 0x5250 is an arbitrary unused id (system ones stop at 127); the deps
// {fs, nifm} put us down before them on sleep and back up after them on wake.

PscPmModule g_pscModule;
Thread g_pscThread;
bool g_pscReady = false;
volatile bool g_pscThreadRun = true;
volatile bool g_wokeFromSleep = false;
volatile bool g_sleepPending = false;
volatile bool g_asleep = false;
UEvent g_pscWakeMain;
UEvent g_sleepPrepDone;

void pscThreadFunc(void* arg) {
    (void)arg;

    while (g_pscThreadRun) {
        // 1s timeout so shutdown isn't blocked waiting for a power transition
        if (R_FAILED(eventWait(&g_pscModule.event, 1000000000ULL))) continue;

        PscPmState state;
        u32 flags = 0;
        if (R_FAILED(pscPmModuleGetRequest(&g_pscModule, &state, &flags))) continue;

        if (state == PscPmState_ReadySleep) {
            // Let the main thread flush a final heartbeat while wifi is still
            // up, wait briefly, then ack — never stall the sleep for long.
            g_sleepPending = true;
            ueventSignal(&g_pscWakeMain);
            waitSingle(waiterForUEvent(&g_sleepPrepDone), 4000000000ULL);  // up to 4s
        } else if (state == PscPmState_Awake) {
            g_wokeFromSleep = true;
            ueventSignal(&g_pscWakeMain);
        }

        pscPmModuleAcknowledge(&g_pscModule, state);
    }
}

int64_t g_timeOffset = 0;

u64 getRawNowSec() {
    u64 rawLocal = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &rawLocal)) || rawLocal == 0) {
        rawLocal = (u64)time(NULL);
    }
    return rawLocal;
}

u64 getCorrectedNowSec() {
    return (u64)((int64_t)getRawNowSec() + g_timeOffset);
}

// Both sides of the measured offset are whole seconds — the HTTP `Date` header
// carries no finer resolution, and neither does the console clock — and they are
// sampled a moment apart. Two clocks in perfect agreement therefore still
// measure an offset that flickers between two adjacent integers, decided by
// nothing but where each one happens to sit inside its own second.
//
// Adopting that flicker was enough to store a play session twice. The session's
// corrected end moved one second past the server's watermark, the journal scan
// below counted it as unsent, and it arrived again carrying the shifted
// correction — a second row for one launch, an hour after the first.
//
// Drift is not lost by ignoring a step this small: the comparison is against the
// adopted value rather than the previous measurement, so a clock genuinely
// running away keeps widening the delta until it crosses and is taken in one
// step. That bounds the error at the threshold, and a second on a session
// boundary is worth nothing beside a duplicated session.
static const int64_t CLOCK_OFFSET_HYSTERESIS_SEC = 1;

void updateTimeOffset(int64_t newOffset) {
    int64_t delta = newOffset - g_timeOffset;
    if (delta == 0) return;
    if (llabs(delta) <= CLOCK_OFFSET_HYSTERESIS_SEC) return;

    g_timeOffset = newOffset;
    writeToLog("[Time] Clock offset against the server is now %llds (delta %llds)", (long long)g_timeOffset,
               (long long)delta);
}

u64 parseIso8601(const char* text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text == NULL || sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    // Days from civil, Howard Hinnant's formula: no timegm in newlib.
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const s64 days = (s64)era * 146097 + (s64)doe - 719468;
    return (u64)(days * 86400 + hour * 3600 + minute * 60 + second);
}

void formatIso8601(u64 epochSec, char* out, size_t outSize) {
    time_t t = (time_t)epochSec;
    struct tm utc;

    if (gmtime_r(&t, &utc) == NULL) {
        snprintf(out, outSize, "1970-01-01T00:00:00Z");
        return;
    }

    snprintf(out, outSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
}

void formatPlaySpan(int seconds, char* out, size_t outSize) {
    if (seconds < 60) {
        snprintf(out, outSize, "< 1 мин");
        return;
    }
    const int minutes = seconds / 60;
    if (minutes < 60) {
        snprintf(out, outSize, "%d мин", minutes);
        return;
    }
    snprintf(out, outSize, "%d ч %d мин", minutes / 60, minutes % 60);
}
